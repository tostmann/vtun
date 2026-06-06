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

- **Long-working clients suddenly reconnect-loop after the server moved to a modern OS.** The client logs `Input/output error (5)` right after connecting, the server logs `Connection closed by other side`, and the session retries every few seconds — sometimes succeeding after minutes, seemingly at random. The cause is **not** the network: a modern Linux kernel emits a link-local multicast autoconf burst (IPv6 Router Solicitation + MLDv2 and an IPv4 IGMPv3 report) the instant a freshly created `tun` interface comes up. Pristine vtund forwards that burst into the tunnel; if the peer's `up{}` script has not finished yet, the peer writes the frames into its still-DOWN tun, gets `EIO`, and its session dies. Whether a client survives is a pure race against its own `up{}` script. Older kernels never generated this traffic, which is why the same clients worked for years against the old server.

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
- **Never tunnels link-local multicast** (IPv4 `224.0.0.0/24`, IPv6 `ff02::/16`) read from the local tun. This stops the kernel's autoconf burst from killing peers whose `up{}` script is still running (the reconnect-loop described above) — by default, with no config change on either end and no change to the wire protocol. Link-local-scoped multicast must never leave its link anyway; global-scope multicast and all unicast traffic are forwarded unchanged.
- **Logs each message once.** Pristine vtund's `openlog(..., LOG_PERROR, ...)` writes every syslog message to stderr as well; under systemd (foreground `-n`) the journal captured both copies, doubling every line. The fork drops `LOG_PERROR`.
- **Creates the runtime lock directory via `tmpfiles.d`** (`/run/lock/vtund`). `/var/lock` is a tmpfs on modern systems; without the directory, server-side authentication fails with `Can't create temp lock file`.

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

## Install via apt (recommended)

There is a **signed APT repository** on GitHub Pages, so you can install and stay up to date with `apt`. The snippet below picks the right suite for your system from `/etc/os-release`:

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://tostmann.github.io/vtun/repo-signing-key.asc \
  | sudo tee /etc/apt/keyrings/vtun.asc >/dev/null
echo "deb [signed-by=/etc/apt/keyrings/vtun.asc] https://tostmann.github.io/vtun/$(. /etc/os-release && echo "$VERSION_CODENAME")/ ./" \
  | sudo tee /etc/apt/sources.list.d/vtun.list >/dev/null
sudo apt update && sudo apt install vtun
```

Supported suites: **bookworm** (Debian 12), **trixie** (Debian 13), **jammy** (Ubuntu 22.04), **noble** (Ubuntu 24.04) — each for **amd64** and **arm64**.

## Install a single .deb manually

Prebuilt `.deb` packages are attached to the **[GitHub Releases](../../releases)** for:

- Debian 12 (bookworm) and Debian 13 (trixie)
- Ubuntu 22.04 (jammy) and Ubuntu 24.04 (noble)
- architectures: **amd64** and **arm64**

Download the package matching your distribution and architecture, then:

```sh
sudo apt install ./vtun_*_amd64.deb     # or the matching arm64 file
```

## Run as a service (systemd)

The package ships two native systemd units. They are installed **disabled** (the package never starts a tunnel on its own) — configure first, then enable:

- `vtund.service` — the standalone **server** (hub); reads `/etc/vtund.conf`.
- `vtund-client@.service` — a **client** template, one instance per session.

**Server / hub:**

```sh
sudoedit /etc/vtund.conf            # define your sessions
sudo systemctl enable --now vtund   # start now and on boot
sudo systemctl reload vtund         # re-read the config (SIGHUP), no tunnel drop
sudo systemctl status vtund
```

**Client session** — e.g. a session called `office` (defined in `/etc/vtund.conf`) that dials a server at `vpn.example.org`:

```sh
echo 'VTUN_PEER=vpn.example.org' | sudo tee /etc/default/vtund-client@office
sudo systemctl enable --now vtund-client@office
sudo systemctl status vtund-client@office
```

The units run `vtund` in the foreground (`-n`) so systemd supervises it (auto-restart on failure). `systemctl reload` sends `SIGHUP`, which makes vtund re-read `/etc/vtund.conf`.

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
