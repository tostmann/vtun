# PATCHES

Every change this fork makes versus pristine upstream **vtun 3.0.4** (Maxim Krasnyansky, last upstream release 2016), grouped by topic. The goal of the fork is narrow: make vtun / `vtund` **build and run** on modern Linux — OpenSSL 3.x, GCC 14+, Debian 13 "trixie", Ubuntu 24.04 — without changing the on-the-wire protocol.

For each topic: the file(s) touched, the concrete failure mode on pristine 3.0.4, and the fix applied here.

---

## 1. OpenSSL 3 legacy-provider load — the segfault fix

**File(s):** `main.c`

**What was wrong**

Blowfish is vtun's *default* cipher: `encrypt yes` maps to `blowfish128ecb`. OpenSSL 3 relocated Blowfish (and the other "legacy" ciphers) into the **legacy provider**, which is **not loaded by default**. Pristine vtun calls

```c
EVP_EncryptInit_ex(ctx, EVP_bf_ecb(), NULL, key, NULL);
```

On OpenSSL 3 `EVP_bf_ecb()` resolves to a cipher whose implementation lives in an unloaded provider, the init call fails, **the return value is ignored**, and the code then dereferences the half-initialised cipher context → **SIGSEGV** inside `libcrypto.so.3`.

Because `vtund` forks one child process per session, only the *child* dies. The master keeps printing `waiting for connections on port ...`, so the crash is **invisible in normal logs** and merely looks like "clients randomly fail to connect / sessions drop." The kernel log shows:

```
vtund[NNN]: segfault at ... in libcrypto.so.3[...]
```

**The fix**

In `main.c`, on startup, explicitly load the legacy provider in-process via `OSSL_PROVIDER_load()` (and keep the default provider loaded), guarded for `OPENSSL_VERSION_NUMBER >= 0x30000000L` so OpenSSL 1.1.x builds are unaffected:

```c
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
    OSSL_PROVIDER_load(NULL, "legacy");
    OSSL_PROVIDER_load(NULL, "default");
#endif
```

This is done **before daemonizing**: `vtund`'s `daemonize()` strips the environment, so steering provider loading via `OPENSSL_CONF=...` in the environment is unreliable. Loading the provider in-process before the fork means the loaded providers are **inherited by the forked per-session children**, so every Blowfish session has the legacy cipher available instead of crashing.

Runtime requirement: the legacy provider module (`ossl-modules/legacy.so`) must exist on disk — it ships with the standard `openssl` / `libssl3` packages on Debian/Ubuntu, so normally nothing extra is installed. AES sessions need only the default provider.

---

## 2. EVP_CIPHER_CTX opaque-context port

**File(s):** `lib/lfd_encrypt.c`

**What was wrong**

OpenSSL 1.1 made `EVP_CIPHER_CTX` **opaque**: it can no longer be declared by value or stack-allocated, and `EVP_CIPHER_CTX_init()` / `EVP_CIPHER_CTX_cleanup()` were removed. Pristine vtun 3.0.4 embeds the context by value and uses the old init/cleanup API, so it does **not compile** against OpenSSL 1.1+ / 3.x (incomplete-type / unknown-symbol errors).

**The fix**

Port the cipher context to the heap-allocated opaque API:

- declare the context as a pointer `EVP_CIPHER_CTX *ctx;`
- allocate with `EVP_CIPHER_CTX_new()`
- between operations use `EVP_CIPHER_CTX_reset()` (the 1.1+ replacement for the removed `_init` / `_cleanup`)
- free with `EVP_CIPHER_CTX_free()`

This is the standard OpenSSL 1.1+/3.x opaque-context pattern and is the minimal change needed for `lfd_encrypt.c` to compile and run on modern libcrypto.

---

## 3. `arpa/inet.h` / `htonl()` prototype — fixes CBC / CFB / OFB (non-ECB) modes

**File(s):** `lib/lfd_encrypt.c`

**What was wrong**

`lfd_encrypt.c` uses `htonl()` to serialise the per-packet sideband (the IV / sequence number used by the chained, non-ECB modes) but did **not** include `<arpa/inet.h>`. Without the prototype in scope, the compiler assumed the pre-C99 default of `int htonl()` — i.e. an implicit `int` return. On a 64-bit LP64 target the 32-bit `int` return is **not** correctly widened/handled relative to the real `uint32_t` prototype, so the serialised IV / sequence value was **corrupted**.

The practical symptom: only **ECB** modes worked (ECB does not chain across blocks and does not depend on this sideband value), while **CBC / CFB / OFB** modes produced garbage / failed to decrypt. This is easy to misread as "only the default cipher works."

**The fix**

Add the missing include so `htonl()` (and the byte-order family) is correctly prototyped:

```c
#include <arpa/inet.h>
```

With the real prototype in scope the IV / sequence number serialises correctly on 64-bit, restoring CBC / CFB / OFB.

---

## 4. GCC 14 / C99 build fixes

GCC 14 promotes **implicit function declarations** (`-Werror=implicit-function-declaration`) and **implicit `int`** to hard errors, and defaults to **`-fno-common`**. Pristine vtun 3.0.4 relies on all three of the old permissive behaviours and therefore **fails to build** (FTBFS). The fixes:

**Missing prototype includes**

- **`lib.h`** — add `#include <unistd.h>` (for `read`/`write`/`close`/`fork`/`getpid`/etc. used throughout).
- **`lock.c`** and **`lfd_shaper.c`** — add `#include <time.h>` (for `time()` and related time prototypes).
- **`pty_dev.c`** — add `#define _GNU_SOURCE` (before any include) and `#include <stdlib.h>`. `_GNU_SOURCE` exposes the GNU pty helpers (`grantpt` / `unlockpt` / `ptsname` and `getpt`-family declarations) and `<stdlib.h>` provides their prototypes plus `malloc`/`free`/`exit`. Without these the calls were implicitly declared → GCC 14 error.

**C99 `inline` fix for `clear_nat_hack_flags`**

- **`vtun.h`** and **`cfg_file.y`** — `clear_nat_hack_flags()` was defined as a bare `inline` function in a header included by multiple translation units. Under the **C99** inline semantics that GCC 14 now applies (vs. the old GNU89 `extern inline` behaviour), a plain `inline` definition in a header emits **no external symbol**, while a non-inline use produces an **undefined-reference** link error (and duplicate-definition issues across TUs). Fixed by adjusting the linkage so exactly one external definition is emitted (C99-correct `inline` / `extern inline` handling) — the same name is referenced from the generated parser in `cfg_file.y`.

**`-fcommon` for legacy common-symbol globals**

- **build flags (`configure` / generated `Makefile`)** — vtun declares several globals (tentative definitions) in headers included by multiple `.c` files, relying on the historical "common block" merge. GCC 14 defaults to `-fno-common`, turning these into **multiple-definition** link errors. The fork restores the legacy behaviour by adding **`-fcommon`** to the compile flags rather than rewriting every global to `extern` + one definition.

---

## 5. `HAVE_WORKING_FORK` detection fallback

**File(s):** `compat.h` (with the autoconf `HAVE_WORKING_FORK` feature test)

**What was wrong**

The autoconf test for a working `fork()` **mis-detected** on modern toolchains/build environments and concluded that `fork()` did not work. `HAVE_WORKING_FORK` being unset silently **compiled out**:

- the **standalone server (`-s`) mode**, and
- the **up/down-script fork path** (running the per-session up/down shell hooks).

The result was a binary that built fine but was missing standalone-server operation and script execution, with no obvious error — just "it doesn't run as a server" / "my up/down scripts never fire."

**The fix**

Add a fallback in `compat.h` so that on a normal POSIX system `HAVE_WORKING_FORK` is **defined** (defaulting to assume a working `fork()` unless the platform genuinely lacks it), rather than trusting the mis-detecting configure test. This re-enables `-s` standalone-server mode and the up/down-script fork path.

---

## 6. Refreshed `config.guess` / `config.sub` for aarch64 / arm64

**File(s):** `config.guess`, `config.sub`

**What was wrong**

The `config.guess` / `config.sub` shipped with vtun 3.0.4 (2016) predate widespread 64-bit ARM support and do **not** recognise `aarch64` / `arm64` host triplets. On an arm64 machine `./configure` aborts early with a "cannot guess build type" / "unrecognized host" error before any compilation begins.

**The fix**

Replace both files with up-to-date GNU `config.guess` / `config.sub`, which know the `aarch64-*-linux-gnu` (and related arm64) triplets, so `./configure` succeeds on aarch64 / arm64. No functional code change — purely the autoconf canonicalisation helpers.

---

## 7. Link-local multicast filter — fixes the reconnect-loop against modern hubs

**File(s):** `linkfd.c`

**What was wrong**

A modern Linux kernel, the instant a freshly created `tun` interface is brought UP by the `up{}` script, emits a link-local multicast autoconf burst: IPv6 **Router Solicitation** and **MLDv2 reports** (to `ff02::/16`) plus an IPv4 **IGMPv3 report** (to `224.0.0.22`). Pristine vtund's `lfd_linker()` reads whatever appears on its tun and forwards it into the tunnel.

On a routed point-to-point tunnel this traffic serves no purpose — and it is actively harmful: if the **peer's** `up{}` script has not finished yet, the peer receives the burst and `dev_write()`s it into its **still-DOWN** tun. The kernel returns `EIO`, the peer's linker loop treats that as fatal, the session dies (`Input/output error (5)` on the peer, `Connection closed by other side` on the sender) and reconnects — re-triggering the same burst. Whether a client survives is a pure race between its `up{}` script and the burst's arrival. Fast clients connect first try; slow ones (e.g. small ARM boards running interpreted `up{}` helpers) loop for minutes.

Older kernels never generated this multicast on tun interfaces, which is why long-working deployments broke **only after the server moved to a current OS** — the classic symptom is "all clients worked for years, we replaced the hub, now some clients reconnect-loop."

Note that disabling IPv6 on the tun is **not** sufficient: the IPv4 IGMPv3 report alone also kills the race-losing peer. Clearing `IFF_MULTICAST` on the tun at open time empirically does not suppress the burst either.

**The fix**

In `linkfd.c`, a small filter (`lfd_drop_frame()`) in the TUN→NET path drops frames whose destination is **link-local-scoped multicast** — IPv4 `224.0.0.0/24` (the Local Network Control Block, RFC 5771) or IPv6 `ff02::/16` (link-local scope) — immediately after `dev_read()`, before compression/encryption/`proto_write()`. The tun runs with `IFF_NO_PI`, so the IP version nibble and the destination address are at fixed offsets in the frame.

Link-local-scoped multicast must never be forwarded off its link, so dropping it is correct by definition, not a workaround. Global-/admin-scope multicast (`224.0.1.0` and up) and all unicast traffic are forwarded unchanged; nothing about the wire protocol changes, and the fix is active by default on both ends with no configuration.

---

## 8. Single-copy logging — drop `LOG_PERROR`

**File(s):** `main.c`

**What was wrong**

Both `openlog()` calls passed `LOG_PERROR`, which makes glibc write every syslog message to **stderr** as well. Run in the foreground under systemd (`vtund -n`, as the shipped units do), the journal captures the syslog socket **and** stderr — so **every log line appeared twice**, with confusingly different idents (vtund rewrites `argv[0]` per session for `ps`, and the syslog-socket copy follows that title while the stderr copy keeps the unit ident).

**The fix**

Drop `LOG_PERROR` from both `openlog()` calls. Each message is logged exactly once, via syslog.

---

## Notes

- License unchanged: **GPL-2.0**; upstream copyright preserved — *Copyright (C) 1998-2016 Maxim Krasnyansky*.
- The fork changes **no protocol behaviour**: same `vtund` binary (sbin), same man pages (`vtund.8`, `vtund.conf.5`), same example `vtund.conf`. It only makes the existing protocol build and run on OpenSSL 3.x, GCC 14+, and current Debian/Ubuntu.
- For **new** deployments, prefer `encrypt aes256cbc` over the Blowfish default: Blowfish's 64-bit block is exposed to Sweet32 (CVE-2016-2183) on long-lived high-volume tunnels, and the default ECB mode leaks repeated plaintext blocks. vtun has no AEAD, no message authentication and no forward secrecy; greenfield deployments are better served by a modern VPN such as WireGuard. This fork's purpose is to keep **existing** vtun deployments alive on modern systems.
