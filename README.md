# vtun — VTun / vtund fixed for OpenSSL 3, GCC 14 and Debian 13 (trixie)

A maintained fork of **VTun** (the `vtund` Virtual Tunnel daemon) that builds and runs on modern Linux — **OpenSSL 3.x**, **GCC 14+**, **Debian 13 "trixie"** and **Ubuntu 24.04** — and, in particular, fixes the **segfault in `libcrypto.so.3`** that kills every Blowfish-encrypted session.

## The problem

If you run upstream vtun / vtund on a current system, you have probably hit one or more of these. These are the exact symptoms people search for:

- **vtund segfaults in `libcrypto.so.3` as soon as an encrypted session connects.** The kernel log shows a line like:

  ```
  vtund[NNN]: segfault at ... ip ... sp ... error 4 in libcrypto.so.3[...]
  ```

  The cause is Blowfish, which is vtun's **default** cipher: `encrypt yes` means `blowfish128ecb`. OpenSSL 3 moved Blowfish (and the other legacy ciphers) into the **"legacy" provider**, which is **not loaded by default**. vtun calls `EVP_EncryptInit_ex(ctx, EVP_bf_ecb(), ...)`, the call fails because the cipher is unavailable, the return value is ignored, and the code then dereferences the half-initialised cipher context → **SIGSEGV**.

- **Sessions silently drop / "clients randomly fail to connect", while the server log looks fine.** Because `vtund` forks one child process per session, only the *child* dies on the crash. The master process keeps printing:

  ```
  vtund[...]: waiting for connections on port 5000
  ```

  so the crash is **invisible in the normal logs** — it just looks like clients can't establish or keep a tunnel. This is the same root cause as the `libcrypto.so.3` segfault above.

- **vtun was removed from Debian 13 (trixie).** It is no longer in the current Debian / Ubuntu package archives, so `apt install vtun` no longer works.

- **Upstream vtun 3.0.4 fails to build (FTBFS) against OpenSSL 3 and GCC 14.** OpenSSL 3 made `EVP_CIPHER_CTX` opaque, and GCC 14 promotes **implicit function declarations** to errors and defaults to **`-fno-common`**. The pristine 3.0.4 sources (last upstream release, 2016) do not compile on either.

If any of that matches what you are seeing, this fork is what you are looking for.

## What this fork fixes

- **Loads the OpenSSL 3 "legacy" provider in-process** via `OSSL_PROVIDER_load()`, so `encrypt yes` (Blowfish) works instead of crashing. This is done **before daemonizing** — vtund's `daemonize()` strips the environment, so relying on `OPENSSL_CONF=...` is unreliable — and the loaded provider is inherited by the forked per-session children. Guarded for OpenSSL ≥ 3 only.
- **Ports `EVP_CIPHER_CTX` from by-value to heap-allocated** (`EVP_CIPHER_CTX_new()` / `EVP_CIPHER_CTX_reset()`) for the opaque-context API in OpenSSL 1.1+ / 3.x.
- **Fixes the non-ECB cipher modes (CBC / CFB / OFB).** Adds `<arpa/inet.h>` so `htonl()` is correctly prototyped — without the prototype the sideband IV / sequence number was corrupted on 64-bit, and in practice only the ECB modes worked.
- **Compiles cleanly on GCC 14 / modern glibc:** proper prototype includes (`unistd.h`, `time.h`, `stdlib.h`, plus `_GNU_SOURCE`), `-fcommon` for the legacy common-symbol globals, and a C99 `inline` fix.
- **Fixes `HAVE_WORKING_FORK` detection.** The autoconf test mis-detected, which silently compiled out the standalone server (`-s`) mode and the up/down-script fork path.
- **Refreshes `config.guess` / `config.sub`** so `./configure` works on **aarch64 / arm64**.

## Build from source

Install the build dependencies (Debian / Ubuntu package names):

```sh
sudo apt install gcc make libssl-dev liblzo2-dev zlib1g-dev flex bison
```

`libssl-dev` must be OpenSSL ≥ 3. Then build:

```sh
./configure
make
sudo make install   # optional
```

This installs `vtund` (in `sbin`), the man pages `vtund.8` and `vtund.conf.5`, and the example `vtund.conf`.

**Runtime note:** for Blowfish sessions the OpenSSL **"legacy" provider** (`ossl-modules/legacy.so`) must be present at runtime. It ships as part of the standard `openssl` / `libssl3` package on Debian and Ubuntu, so there is normally **nothing extra to install**. AES sessions need only the default provider.

## Install (.deb)

Prebuilt `.deb` packages are attached to the **[GitHub Releases](../../releases)** for:

- Debian 12 (bookworm) and Debian 13 (trixie)
- Ubuntu 22.04 (jammy) and Ubuntu 24.04 (noble)
- architectures: **amd64** and **arm64**

Download the package matching your distribution and architecture, then:

```sh
sudo apt install ./vtun_*_amd64.deb     # or the matching arm64 file
```

## Encryption & security

Supported ciphers: **Blowfish** and **AES**, with **128-** and **256-bit** keys, in **ECB / CBC / CFB / OFB** modes.

| `encrypt` value      | Cipher / mode             |
|----------------------|---------------------------|
| `no`                 | none                      |
| `yes`                | `blowfish128ecb` (default)|
| `blowfish128ecb`     | Blowfish, 128-bit, ECB    |
| `blowfish256cbc`     | Blowfish, 256-bit, CBC    |
| `aes128cbc`          | AES, 128-bit, CBC         |
| `aes256cbc`          | AES, 256-bit, CBC         |
| …                    | other Blowfish/AES + ECB/CBC/CFB/OFB combinations |

**Honest caveat — read before relying on the defaults:**

- **Blowfish has a 64-bit block size** and is therefore susceptible to **Sweet32 (CVE-2016-2183)** on long-lived, high-volume tunnels.
- The **default uses ECB mode**, which leaks repeated plaintext blocks.
- For **new** configurations, prefer **`encrypt aes256cbc`**.
- vtun has **no AEAD, no message authentication and no forward secrecy**. For a **greenfield** deployment, a modern VPN such as **[WireGuard](https://www.wireguard.com/)** is the better choice.

This fork exists to keep **existing** vtun deployments working on modern systems — in particular hubs whose legacy peers can only speak vtun and cannot be changed. It does not modernise the protocol; it makes the protocol vtun already has run again on OpenSSL 3, GCC 14 and current Debian / Ubuntu.

## Credits

A fork of **VTun** by **Maxim Krasnyansky** — <https://vtun.sourceforge.net/>.

Copyright (C) 1998-2016 Maxim Krasnyansky. Licensed under the **GNU General Public License, version 2.0 (GPL-2.0)**; the upstream copyright is preserved.

Fork maintainer: Dirk Tostmann <tostmann@gmail.com>.
