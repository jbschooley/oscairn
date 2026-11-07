# RFC 0039: Network security

Status: proposed, 2026-11-07. Required for every product Oscairn runs on that has a network
port or radio. Builds on network control and the web UI
([RFC 0026](0026-network-control-and-web-ui.md)), which already decides the transport, pairing,
pinned certificates and permissions; this RFC fills in what 0026 leaves to it and does not
reopen those decisions. Also builds on the control API ([RFC 0003](0003-control-api.md)) and its
[local transport](../control-protocol.md#local-transport-and-client), signatures and the trust
store ([RFC 0006](0006-packages-and-portability.md#signatures)), apps and their permissions
([RFC 0022](0022-apps-and-requests.md#permissions)), multi-device setlists
([RFC 0012](0012-sync-external-midi-and-multi-device.md#network-control-and-safety)), reliability
levels ([RFC 0027](0027-reliability-levels.md)), the system image
([RFC 0028](0028-system-image-updates-boot-and-power.md)) and repository fetching
([RFC 0031](0031-package-repository-and-distribution.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. A TLS library in the image; HTTPS package fetching on Linux
      (built 2026-11-07: Buildroot's Mbed TLS 3.6.7, libcurl 8.22.0 built against it,
      `ca-certificates` and the `curl` tool in the QEMU image; `JUCE_USE_CURL` for the app, the
      engine and the web test on Linux, with certificates always verified. `control-repository-web`
      takes `OSCAIRN_TEST_CA`, a test authority the system trusts, and then runs over HTTPS and
      refuses a wrong host name; it always checks that an unknown authority is refused. Passed
      with the image's own binaries and libraries in a Linux container that trusts a test
      authority, and, on macOS, the refusal alone. In the booted QEMU image with a network card,
      curl and the web test binary fetched over HTTPS from a test authority's server, and curl
      from a public site with the image's bundle; an untrusted certificate and a wrong host name
      were refused. Size [below](#the-tls-library). Not yet, so the gate is not met:
      `control-repository-web` run inside the booted image, which has no Python; the
      last-known-time rule ([below](#https-fetching-and-the-clock)); the trusted case on macOS,
      which needs the test authority in the keychain; Mbed TLS 4.1, an open question)
- [ ] 2. The firewall: default deny, rules generated from the enabled services
- [ ] 3. The network gateway: a process apart from the engine, WebSocket over TLS, limits
- [ ] 4. Pairing, client keys, revocation and sessions
- [ ] 5. Discovery, and the other listeners (OSC, Link, network MIDI) under the same rules
- [ ] 6. Robustness measured: a LAN flood and malformed input while playing, no missed deadlines
- [ ] 7. Keys at rest, and network services patched through system updates

## Problem

RFC 0026 puts the control API on the network and decides how in outline: WebSocket over TLS,
off by default and per interface, pairing confirmed on the device, a certificate made on the
device and pinned by its fingerprint, and four permissions by namespace. It does not say what
the device is defending against, which other services listen beside it, how a browser (which
cannot pin a certificate) fits that model, which TLS library the image carries, what stops a
flood from the venue's Wi-Fi reaching the sound, or how a hole in any of this gets fixed on an
instrument that may never see the internet.

Some of that is needed already: RFC 0031's HTTPS fetching works on macOS and not on Linux,
because the image has no TLS library.

## Outcome

- An Oscairn device on a stage or home network exposes nothing until its owner turns something
  on, then exposes only that, on the interfaces chosen, behind a packet filter that drops the
  rest.
- Every network client that can change anything is one the owner paired, with the permissions
  given, and can be revoked from the device at any time.
- Nothing arriving from the network, well formed or not, at any rate the network can carry,
  costs the audio a block.
- One TLS library, chosen for size, maintenance and Buildroot support, serves the web UI, the
  control API and package fetching, and is patched by system updates like the rest of the image.
- Where the design cannot protect something (a browser's first visit, a device in a stranger's
  hands), it says so to the user instead of pretending.

## Current state

- **The image listens on nothing.** The Buildroot defconfig
  (`os/buildroot/configs/oscairn_qemu_aarch64_defconfig`) enables no network service package,
  and Buildroot 2026.08.1's default BusyBox configuration has `telnetd` and `httpd` off (it has
  `inetd` and `udhcpc` built, with no service started for `inetd`). The QEMU image runs with
  `-nic none` (`os/build.sh`, `os/scripts/boot-test.py`), so it has never had a network.
- **The defconfig sets no root password** (`BR2_TARGET_GENERIC_ROOT_PASSWD` is not set), so the
  console takes a root login with no password. Fine for a development image; not for a product.
- **The local socket** (`core/control/src/local_socket.cpp`) is a Unix-domain stream, 0600 in a
  0700 directory, same-user only, and "not a sandbox for untrusted clients" (control protocol).
  Its limits: at most 8 clients (`maxClients`), 16 documents dispatched per client per tick
  (`maxRequestsPerTick`), about 64 KiB read and written per client per tick, 1 MiB of queued
  output per client (`maxQueuedBytes`); input of at most 65,536 bytes, 32 levels of nesting and
  64 requests per batch, checked before parsing. An oversized frame, an I/O failure or a full
  output queue disconnects that client; malformed JSON gets an error and later frames are still
  read. Requests run synchronously on the control thread, which also runs plugin main-thread
  work; plugin loading there stalls other clients' controls, though not the audio (RFC 0022
  plans moving those off the thread).
- **There is no TCP, WebSocket or TLS code** in the engine. Package fetching uses JUCE's URL
  streams: the system's HTTPS on macOS, libcurl with Mbed TLS on Linux (RFC 0031, step 2; step 1
  here).
- **Signatures do not depend on TLS.** Repositories, packages, builds and apps are checked by
  Ed25519 signatures (Monocypher) against the trust store, and blobs by SHA-256 (RFC 0006,
  RFC 0031). TLS on a fetch adds privacy and protects what is not signed (which packages a
  device asks for); it is not what makes a package trustworthy.
- **RFC 0026's permissions** (Play, Edit, Library, System) are designed, not built. RFC 0022
  reuses them for apps and adds six app-only ones; its step 4 (grants per connection, rate
  limits) is not built either.
- RFC 0035 (the system base, including tracking CVEs) and RFC 0037 (which may add file shares)
  are being written elsewhere and are not in the repository yet; this RFC refers to them by
  number only.

## Threat model

Who can reach the device, what they can do today, and what this RFC must hold against.

| Who | Reaches | Wants, or does by accident | Must hold |
| --- | --- | --- | --- |
| The audience, on shared venue Wi-Fi | Every port the device opens on that network; broadcast and multicast (mDNS, Link) | Curiosity; a prank (change the tempo, play notes, load a Performance); a flood, by malice or a broken app | Nothing changes without pairing; no pairing request reaches the owner's screen unless the owner opened pairing; no flood costs a block |
| Other devices on the LAN | The same, plus long-lived presence: a compromised laptop, a smart TV, a router | Scanning, exploiting a parser bug, impersonating the device to a client (a man in the middle) | As above; a paired native client detects an impersonated device; a parser bug in network code does not reach the engine |
| A paired client used beyond its grant | The control API, with its permissions | A stage tablet given Play trying Edit or System | The engine refuses outside the grant (RFC 0026); revocation closes it at once |
| A malicious package or app | The engine as a plugin (in a worker, RFC 0010) or as a client (RFC 0022); with `network`, the network | Reading the device's keys; adding its key to the trust store; opening a listener; reaching other LAN hosts | Keys unreadable to workers and apps; the trust store writable only through `system`; no listener it can open passes the firewall; outbound traffic limited to what was granted |
| Physical access | The console, USB, the storage card | Anything | Stated, not solved in general: whoever holds the device and its storage holds what is on it, unless the product adds verified boot and encrypted storage ([Keys](#updates-and-keys)) |

**Out of scope:** the internet. RFC 0026 rules out remote access beyond the local network
without its own RFC, and nothing here forwards ports or talks to a relay. MIDI arriving on a
port stays trusted as the musician's own cabling (RFC 0012).

**What matters most** is the sound. A device that leaks its library listing is embarrassing; one
that goes silent mid-song because a phone on the venue's Wi-Fi sent junk has failed at its job.
Robustness ([below](#rate-limits-and-robustness)) is weighed as a security property.

## Exposure

Every network service, its default, and where it may listen.

| Service | Transport | Default | Who may use it | Per interface |
| --- | --- | --- | --- | --- |
| Control API and web UI (RFC 0026) | TCP: HTTPS and WebSocket over TLS, one port | Off | Paired clients; the pairing exchange only, while pairing is open | Yes |
| Discovery (RFC 0026) | UDP multicast DNS (mDNS / DNS-SD) | Off; on only while the control API is on | Anyone on that network sees the device's name and profile | Follows the control API's interfaces |
| Library sync, a paired computer as a repository mirror (RFC 0026, RFC 0031) | The control API's connection | With the control API | Paired clients with Library | Same |
| Multi-device setlists (RFC 0012) | The control API's connection | Off | Paired devices, with the grant [below](#mapping-to-rfc-0026s-permissions) | Same |
| OSC bridge (RFC 0003) | UDP | Off | See [the conflict](#the-osc-bridge-and-rfc-0026) | Yes, with a source allow-list |
| Ableton Link (RFC 0012) | UDP multicast | Off | Any Link peer on that network | Yes |
| Network MIDI (RFC 0012's network session; RTP-MIDI if built) | UDP | Off; not designed yet | Peers of the named session | Yes |
| File shares, if RFC 0037 adds them | TCP | Off | Paired, with Library | Yes |
| A shell (SSH) | TCP | Absent from product images | Development images only, by key | Yes |
| Outbound: package fetching, time | TCP (HTTPS), UDP (NTP) | Only when asked or when the user enabled it | The device itself | Any route |

Rules:

- **Off by default, all of it.** A new device, a factory reset, or a reset of network settings
  leaves every listener off. Turning one on is a setting on the device (or from a paired client
  with System), shown in the top bar while on (RFC 0030 already reserves a network indicator).
- **Per interface.** Each service lists the interfaces it listens on: wired, Wi-Fi client, the
  device's own access point if a product has one, USB networking to a computer. A keyboard can
  serve the web UI on the stage's wired network or a USB cable and nothing on the venue's Wi-Fi.
  An interface that is not chosen gets nothing: the service binds to that interface's addresses
  only, and the firewall drops the rest anyway, so either one alone keeps it closed.
- **One port for the control API and the web UI.** The web UI's files and the WebSocket share
  one TLS listener, so there is one thing to open, one certificate and one origin.
- **mDNS says little.** The device's name (as the owner set it) and its profile ID (RFC 0024),
  no serial number, owner name or software version. A product with discovery off is still
  reachable by address.
- **Unauthenticated services are labeled so.** Link, the OSC bridge and network MIDI have no
  pairing in their protocols as Oscairn would use them. Turning one on says that anyone on the
  chosen networks can use it, and recommends a wired or private network.

### Ableton Link

Any Link peer on a network can change the session's tempo, and every peer follows it. On venue
Wi-Fi that means any phone running a Link app. Link stays off by default and is chosen per
interface like the rest; the setting says that any device on that network can change the tempo.
Whether Oscairn offers a "follow only on this network, lead on that one" choice is
[open](#open-questions).

### The OSC bridge and RFC 0026

RFC 0003 plans an OSC bridge on UDP, off until enabled, offering parameters, transport and notes.
RFC 0026 later rules out "unauthenticated network control, even just for the local network",
and OSC as TouchOSC, Open Stage Control and Max speak it has no authentication. The two disagree.
Options:

1. **Drop the UDP bridge;** OSC controllers reach Oscairn through a paired client that
   translates (a desktop helper), or not at all.
2. **Keep it as an explicit exception:** off by default, per interface, only from source
   addresses the owner lists, only Play's methods, and labeled unauthenticated. A source address
   on a LAN is easy to forge for UDP, so the allow-list stops accidents, not attackers.
3. **OSC over the paired connection only** (OSC messages inside the WebSocket), which protects it
   but no existing OSC app speaks.

Recommended: 2, with RFC 0026's rule amended to name it as the one exception, because OSC
controllers are an established part of live rigs and RFC 0003 already promised them. The
owner decides ([open questions](#open-questions)).

## Authentication and pairing

RFC 0026 decides: paired clients only; "a new client shows a code; the owner confirms it on the
device (or, on a headless unit, by a button press or from an already paired client)"; paired
clients keep a key; unpaired ones get nothing beyond the pairing exchange; the device's
certificate is made on the device, shown at pairing as a fingerprint and pinned; no certificate
authority. This section fills in the details and keeps all of that.

### Pairing opens only when the owner opens it

A device with the control API on does not accept pairing requests at all until the owner chooses
**Pair a device** on it (or, headless, holds the button RFC 0026 names, or asks from a paired
client with System). Pairing then stays open for two minutes or until one client is paired. At
other times a pairing request gets a refusal and nothing reaches the screen, so an audience
member cannot flood the owner's screen with requests mid-set. While open, at most four pending
requests are shown, newest dropped beyond that, and each address may have one pending at a time.

### The code

The code the client shows does two jobs, and how well it does the second depends on the client:

- **Which request this is.** The owner confirms the request whose code matches the one on the
  laptop or tablet in front of them, not a stranger's request that arrived at the same moment.
  This holds for every client.
- **That nobody is in the middle.** For a **native client** (the desktop app, a companion app),
  the code is computed on both sides from the device's certificate, the client's public key and
  a nonce from each, so it matches only if both ends see the same keys, as Bluetooth's numeric
  comparison works. The device shows the code it computed beside the request; matching codes
  mean no man in the middle. Six decimal digits, from 20 bits of the hash (a one-in-a-million
  chance for an attacker who must succeed on the first try). A **browser** cannot compute this:
  page scripts cannot see the TLS certificate the browser accepted. For a browser the code only
  identifies the request, and protection against a man in the middle rests on the certificate
  (next section).

### Client keys

- **Native clients** make an Ed25519 key pair at first run (Monocypher, as for signing). Pairing
  records the public key; later connections prove it by mutual TLS (the client's self-signed
  certificate carries the key) or, if the TLS library makes that awkward, by signing a challenge
  from the device bound to the TLS session (an exporter value), decided in step 4.
- **Browsers** make a key with WebCrypto, marked non-extractable, kept in the page's IndexedDB,
  and sign a challenge from the device on each connection. P-256 ECDSA, which every current
  browser's WebCrypto offers. Any script on the page's origin can use the key while it runs
  there, but it cannot be read out and copied to another machine. No cookies: authentication is
  inside the WebSocket, never ambient, so another web site the phone opens cannot ride on it.
- **The device keeps** for each paired client: its name (given at pairing, editable), its public
  key, its permissions, when it was paired and when it was last seen. In the data partition, with
  paired clients as RFC 0028 lists them.

### Sessions

A session is one WebSocket connection, authenticated at its start and ending when it closes.
Reconnecting proves the key again; a challenge signature costs a fraction of a millisecond.
There are no bearer tokens to steal and no session expiry to manage. A browser tab that sleeps
reconnects and proves its key on wake.

### Revocation

The device's network settings list paired clients, with their permissions and when each was last
seen. **Forget** removes one and closes its connections at once; changing its permissions applies
to its open connections at once (the engine re-checks grants per call, so nothing is cached past
the change). A paired client with System can do the same from afar, except remove the last
client with System from a headless unit (which would lock its owner out of everything but a
factory reset). A factory reset that clears user data forgets every client; one that keeps data
keeps them.

### Mapping to RFC 0026's permissions

| Client | Gets | Notes |
| --- | --- | --- |
| A paired browser or native client | Some of Play, Edit, Library, System, chosen at pairing | RFC 0026's table, unchanged. Pairing defaults to Play only; the owner ticks more |
| A paired Oscairn device in a setlist (RFC 0012) | Play, plus readiness and cue methods | RFC 0012: may cue, prepare, switch and report; may not install, read files or change settings. Whether this is a fifth permission or Play plus named methods is [open](#open-questions) |
| An app (RFC 0022) | Its manifest's grants | Local only; never over the network. The app-only permissions (`controls`, `midi`, `audio`, `background`, `network`, `files`) are not offered to network clients |
| Unpaired | `session.hello` and the pairing exchange | RFC 0026 |

- **System over the network installs code** (plugins, apps, system images). RFC 0006's install
  policy still applies in full: unsigned code or an unknown key asks first. When the request
  comes from a network client, that question appears on the device's own screen (or is refused
  on a headless unit unless the client is the owner's, by a setting), not only on the client.
- **The engine enforces grants,** as RFC 0022 already says for apps: per connection, per call,
  an error naming the missing permission. The gateway ([below](#rate-limits-and-robustness))
  authenticates; the engine authorizes.

## Transport security

### The certificate

RFC 0026 decides a self-signed certificate made on the device and pinned by fingerprint at
pairing. Kept. Details:

- An Ed25519 or P-256 key, decided with the TLS library (browsers' TLS support for Ed25519
  certificates is to be checked; P-256 works everywhere). Made at first boot, kept until a
  factory reset that clears data, or until the owner asks for a new one (which unpairs every
  client).
- **Validity dates wide enough to ignore,** because a device may have no clock (RFC 0006,
  RFC 0030): not-before in the past at the time of the build, not-after decades on. Native
  clients pin the fingerprint and ignore the dates.
- **Names:** the device's `.local` name and its addresses, so a browser can connect by either.

### Browsers and certificates no authority signed

A browser warns on a certificate no authority it trusts has signed, and lets the user view the
certificate and continue. That is the weak point of any local web UI, and the options are:

| Option | What protects the first visit | Works offline, at a venue with no internet | Cost |
| --- | --- | --- | --- |
| **Self-signed, user checks the fingerprint** (recommended) | The user comparing the fingerprint the device shows with the one the browser shows; most will not | Yes | A warning on first visit, per browser |
| A per-device public certificate under a project domain (as some media servers do) | A certificate authority | No: issuing needs the internet, the name must resolve to a LAN address through public DNS, which some routers block, and publicly trusted certificates' maximum validity falls to 200 days from March 2026 and 47 days by 2029 under the CA/Browser Forum's ballot SC-081v3, so an offline instrument's certificate would lapse | A service the project runs forever; a dependency RFC 0026's "no cloud account" spirit rules out |
| A device-made certificate authority the user installs on each client | The user installing it once | Yes | Installing a root on a phone is hard and a root key on the device that a thief could use to sign for any name, unless name constraints hold in every client (not verified) |
| Plain HTTP, with security inside the page | Nothing: whoever is in the middle serves the page and its scripts | Yes | Ruled out |

Recommended: self-signed, with the honesty RFC 0026 implies. The device's pairing screen shows
the fingerprint in the form browsers display it, and says to compare it. After the first visit a
browser remembers the exception for that name, and a different certificate later brings the
warning back, which is the same pin a native client keeps, enforced less firmly. **For editing
and anything with System, the native desktop app is the recommended client,** because it pins
properly and its pairing code catches a man in the middle; the web UI is what a phone or a
headless unit needs, and it is protected well against an audience and less well against a
patient attacker on the LAN who is ready when the user first connects.

Unverified, and a gate of step 3: that each of Chrome, Firefox and Safari, on a page served with
an accepted self-signed certificate, gives the page WebCrypto's `subtle` and a working secure
WebSocket. WebCrypto's `subtle` is limited to secure contexts, and whether a certificate
exception still counts as one has not been checked.

### Web UI hardening

- **Origin and Host checked** on every WebSocket upgrade and HTTP request: only the device's own
  names and addresses, so a page on another site cannot open a socket to the device from the
  user's browser (WebSockets are not covered by browsers' cross-origin rules) and a DNS
  rebinding name is refused.
- A strict Content Security Policy (scripts only from the device), no inline scripts, nothing
  fetched from the internet (RFC 0026 already says the web UI comes from the image).
- No WebSocket compression (`permessage-deflate`): it lets a small message expand into a large
  one, and the messages are small JSON already.

### The TLS library

One library for the gateway's server side and for HTTPS fetching (libcurl, so JUCE's
`JUCE_USE_CURL` path works, as RFC 0031 names). Checked against Buildroot 2026.08.1, the version
the image builds (`os/README.md`):

| Library | In Buildroot 2026.08.1 | Version there, and its support | License | libcurl backend in Buildroot | Notes |
| --- | --- | --- | --- | --- | --- |
| Mbed TLS | `mbedtls` | 3.6.7; the 3.6 LTS branch is supported until March 2027; 4.1, the next LTS, until March 2029 (Mbed TLS's `BRANCHES.md`) | Apache-2.0 or GPL-2.0+ | `BR2_PACKAGE_LIBCURL_MBEDTLS` | Built for small devices; features chosen at build time |
| OpenSSL | `libopenssl` | 3.6.5; 3.6 is no longer listed as supported on OpenSSL's release strategy page as of 2026-11-07 (endoflife.date gives 2026-11-01); 3.5 is the LTS, to 2030-04-08 | Apache-2.0 | `BR2_PACKAGE_LIBCURL_OPENSSL` | The most used and audited; the largest |
| wolfSSL | `wolfssl` | 5.9.4 | GPL-3.0+ (commercial licenses sold separately) | `BR2_PACKAGE_LIBCURL_WOLFSSL` | GPL-3.0 combines with Oscairn's AGPL, but a manufacturer shipping its own closed parts beside Oscairn may not want it |
| GnuTLS | `gnutls` | not checked | not checked | `BR2_PACKAGE_LIBCURL_GNUTLS` | Not evaluated further: no size or API advantage found for this use |

Also in Buildroot 2026.08.1 and relevant: `libwebsockets` 4.5.8 (MIT with exceptions; built
against OpenSSL or Mbed TLS), `avahi` 0.8 (LGPL-2.1+) for mDNS, `ca-certificates`, `nftables`.

**Recommended: Mbed TLS,** for size and because it is meant to be configured down to what a
device uses, with the 4.1 LTS as the target (supported to March 2029, against March 2027 for the
3.6 that Buildroot packages today). That means either a newer Buildroot that packages 4.x or our
own package in `os/buildroot/package/`, and checking that libcurl and libwebsockets build
against 4.x; neither is verified. OpenSSL 3.5 LTS is the fallback if Mbed TLS 4 is not usable by
step 1's gate, and the step records the measured size of each in the image, which this RFC does
not claim.

**Measured in step 1** (the QEMU image, 2026-11-07, Buildroot's Mbed TLS 3.6.7, before and after
on the same output tree): the root file system grew from 100,007,028 to 102,276,119 bytes,
2,269,091 bytes (2.2 MiB). `make graph-size` gives `mbedtls` 983,680 bytes, `libcurl` 919,447
(of which the `curl` tool, for development, is 215,032) and `ca-certificates` 180,744; the other
185,220 bytes are elsewhere, presumably Oscairn's binaries now built with libcurl (not checked
file by file). OpenSSL was not measured. Buildroot's `libcurl` names a CA bundle for
OpenSSL and wolfSSL but not for Mbed TLS, and curl's configure looks for none when
cross-compiling, so libcurl would trust nothing; `os/buildroot/external.mk` passes
`--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`.

### HTTPS fetching and the clock

Validating a public certificate checks its dates. A device without a battery-backed clock boots
with a wrong time and every HTTPS fetch fails. Options: fetch the time first (NTP, which is not
authenticated in its plain form); keep the last known time on the data partition and never start
earlier than it or than the image's build date (as `fake-hwclock`-style tools do); or, because
repositories are checked by signatures anyway, skip date checks for repository fetches only.
Recommended: the second, plus NTP when the user has enabled network time, with date checks kept;
a fetch that fails on dates says the time is wrong and offers to set it. The trust bundle is
Buildroot's `ca-certificates`, updated with the image.

**Not built in step 1.** The floor needs the last saved time, which needs RFC 0035's supervisor
and the data partition, and the image has neither, so there is no small place to put it yet. It
waits for RFC 0035's step 6 (the time state and the saved floor), and step 1's gate stays open on
it. Until then a device with no clock fails every HTTPS fetch on dates, and verification is not
weakened to hide that. The QEMU image is not affected: QEMU's `virt` machine has a real-time
clock that gives the host's time.

## The firewall

- **nftables** (Buildroot 2026.08.1 packages 1.1.4, GPL-2.0), with a static base ruleset loaded
  before any interface comes up: input and forward drop by default; loopback, established and
  related traffic, ICMPv6 neighbor discovery and the DHCP client's replies accepted; nothing else.
- **Generated rules** for each enabled service, per interface (`iifname`): the control API's TCP
  port, mDNS on UDP 5353 while discovery is on, Link, OSC and network MIDI ports when enabled,
  OSC limited to its allow-listed sources. The whole ruleset is written and applied as one
  `nft -f` file, which nftables applies as a single transaction, so a half-applied ruleset never
  exists.
- **New-connection limits** in the ruleset: a rate per source address for new TCP connections
  to the control API's port, so a flood of handshakes is dropped in the kernel before any user
  space work.
- **Who writes the rules:** a small privileged helper (`oscairn-netconfig`) that takes a closed
  set of settings (which services, which interfaces, which ports, the OSC allow-list), validates
  them and writes the ruleset. The engine and gateway run unprivileged and ask it; they cannot
  pass it arbitrary rules.
- **Outbound, later:** rules by the sending user (`meta skuid`): the gateway answers, the fetcher
  and NTP may connect out, apps with `network` (RFC 0022) may, plugin workers and other apps may
  not. RFC 0022 already plans seccomp to refuse network sockets to apps without `network`; this
  is a second layer. Step 7, after apps run as their own users.
- **The kernel** must have nf_tables built; whether the QEMU kernel config
  (`board/qemu/aarch64-virt/linux.config` plus `os/buildroot/board/qemu-aarch64/linux.fragment`)
  has it is not checked. Step 2 adds it to the fragment if not.

## Rate limits and robustness

RFC 0027's rule is that the sound comes first. The network side holds it by keeping everything
network-facing out of the engine, and bounding what reaches it.

### The gateway

A process apart from the engine, **`oscairn-gateway`**, terminates TLS, serves the web UI's
files, speaks WebSocket, runs pairing and checks client keys. For each authenticated session it
holds one connection to the engine's local socket, which the engine adopts with that client's
grants, as RFC 0022 describes for apps (a socket pair, one end adopted with grants), and relays
JSON-RPC documents both ways without parsing their methods.

Why a process apart:

- TLS handshakes, HTTP and WebSocket parsing are the code most exposed to junk from strangers.
  A bug there crashes or compromises the gateway, not the engine; the engine's watchdog
  (RFC 0027) never has to act on it, and the gateway is restarted by its supervisor.
- It runs as its own unprivileged user, not in the `audio` group, under Landlock and seccomp as
  RFC 0022 sandboxes apps, on cores apart from those reserved for audio, at a lower CPU weight,
  with a memory limit. It is the only process that can read the device's TLS key.
- The engine's control thread never does a handshake.

The gateway is trusted for one thing: telling the engine which paired client a connection
belongs to. A compromised gateway could claim any paired client's grants. Two limits bound that:
the engine keeps the pairing records and the grants (the gateway sends a client's identity, and
the engine looks up its grants), and the owner can cap what network clients may ever hold (for
example, no System over the network), which the engine applies whatever the gateway says.
Whether the engine should check each client's proof itself, so that a compromised gateway gains
nothing, is [open](#open-questions).

### Limits

| Where | Limit | Proposed |
| --- | --- | --- |
| Kernel (nftables) | New TCP connections per source address | 10 a second, burst 20 |
| Gateway | Sessions in all; connections per source address | 16; 4 |
| Gateway | Time to finish the TLS handshake and authentication; idle time with no ping | 5 s; 60 s |
| Gateway | WebSocket message size; HTTP request headers | 65,536 bytes, the control API's own limit; 16 KiB |
| Gateway | Pairing | Only while open; 4 pending; 1 per address; 5 failed attempts per address lock it out until pairing next opens |
| Engine | Per connection | The local socket's existing limits (16 documents and about 64 KiB a tick, 1 MiB queued output, disconnect when full), unchanged |
| Engine | Clients | Network sessions counted apart from local clients and apps (RFC 0022 already raises the 8-client limit to count apps apart), so a full gateway cannot lock out the device's own UI |
| Engine | Expensive calls (plugin loading, imports, commits, library writes) | RFC 0022's per-app budget, applied per network client too |

Numbers proposed, tuned in step 6 against measurements.

### Malformed input

Everything the engine already checks for the local socket applies, since the gateway relays
documents unchanged: byte, nesting and batch limits before parsing, UTF-8, duplicate members,
ranges and IDs. The gateway adds WebSocket framing checks (masking from clients, control frame
sizes, fragmentation bounded by the message size) and closes the connection on any violation.
Fuzzing: the gateway's HTTP, WebSocket and pairing parsers, and the engine's JSON-RPC adapter,
under a fuzzer in CI (libFuzzer or AFL++), as a gate of steps 3 and 6.

### Floods below user space

A packet flood costs CPU in the kernel (interrupts and soft interrupts) before any filter runs.
On `instrument` and `pass-through` products (RFC 0027) the network interfaces' interrupts are
steered to cores not reserved for audio, as the reserved cores already keep other work off them.
Wi-Fi drivers are the same case. Step 6 measures it: a flood at line rate from another LAN host,
and junk at the control port, while a Performance plays, with deadline misses read from
`session.status`.

## Updates and keys

### Patching network services

- **Through the system image.** The gateway, the TLS library, libcurl, avahi, nftables and the
  kernel are part of the image and are patched by RFC 0028's signed A/B updates, never in place.
  An image update for a security fix is offered like any other: never during a performance,
  never applied without the user's say.
- **Knowing a fix is needed** is RFC 0035's job (the system base and CVE tracking). Buildroot's
  `pkg-stats` reports known CVEs for the packages in a configuration, and its CycloneDX SBOM can
  be checked against the NVD, per its manual; this RFC asks RFC 0035 to cover every package in
  [Exposure](#exposure)'s path and to treat a CVE in one of them as a reason for a release.
- **Picking libraries by support window.** The finding above stands as an example: Buildroot
  2026.08.1 packages OpenSSL 3.6.5, which is past its support on the date of this RFC, and
  Mbed TLS 3.6.7, supported to March 2027. Products pin a TLS library on an LTS branch and
  track it.
- **Offline devices** learn of fixes only with an image they are given (USB, a paired computer,
  RFC 0028). A device that has network control on and has not been updated for a long time says
  so in its network settings, with its image's date, without turning anything off on its own.
- **Turning a service off is the fastest mitigation.** Because everything is per service and per
  interface, a published hole in, say, mDNS can be closed by the user before an image exists.

### The device's own keys

| Key | Where | Readable by | If stolen |
| --- | --- | --- | --- |
| TLS key (new) | Data partition, the gateway's directory, 0600 | The gateway only | An attacker can impersonate the device to its paired clients. Remedy: a new certificate (re-pairs every client) |
| Device signing key (RFC 0006 `Keys/`) | Data partition, 0600 | The engine's user | An attacker can sign as one of the owner's devices. Remedy: revoke it in other devices' trust stores |
| Paired clients' public keys | Data partition | The engine | Public; not secret, but tampering adds a client. Writable only by the engine |
| Trust store (`trust.json`) | Data partition | The engine; changed only through `trust.*` with `system` | Tampering trusts an attacker's code. Writable only by the engine |
| Manufacturer's and project's keys, revocations | The read-only system partition | Everyone | Public |

- Plugin workers (RFC 0010) and apps (RFC 0022) run as other users and their Landlock rules
  exclude all of the above.
- **Physical access defeats file permissions.** The data partition is a journaling file system
  on removable or readable storage (RFC 0028). Whoever has the card can read the keys and edit
  the trust store. Products that need more use their SoC's verified boot and an encrypted data
  partition with its key held in hardware, where the board offers it; which boards do, and how,
  is per product and not checked here. The console's root shell goes from product images (a
  locked root account; a development image keeps it).
- **A lost or stolen device:** its owner forgets it on their other devices and computers (which
  revokes its client key there) and revokes its device signing key in their trust stores.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. TLS in the image | Mbed TLS (4.1 if usable, else the fallback, as above) and libcurl with it; `JUCE_USE_CURL`; `ca-certificates`; the last-known-time rule | `control-repository-web` passes over HTTPS on the Linux image against a server with a certificate from a test authority the image trusts; a fetch with a bad certificate fails; the image's size before and after recorded |
| 2. Firewall | `nftables`, the kernel options, the base ruleset at boot, `oscairn-netconfig` generating per-service, per-interface rules | The QEMU image with a NIC: a port scan from the host finds nothing open with every service off; turning the control API on for one interface opens one port on that interface only; turning it off closes it; a malformed settings request is refused |
| 3. Gateway | `oscairn-gateway`: TLS, the web UI's files, WebSocket, the engine connection per session, Origin and Host checks, the limits table; its sandbox and supervisor | A client over WebSocket reaches the same methods as the local socket; killing the gateway while a Performance plays loses no block and it comes back; a cross-origin page and a rebinding Host are refused; fuzzers run in CI; the three browsers' secure-context check above |
| 4. Pairing | Pairing open only on request; the code, bound to keys for native clients; client keys (native and WebCrypto); sessions; revocation; grants per client in the engine; the network cap on grants | A request outside the pairing window reaches no screen; a native client through a proxy with its own certificate gets a mismatched code; a Play-only client is refused Edit with the permission named; forgetting a client closes its open session within a second |
| 5. Other listeners | mDNS while the control API is on; Link, OSC and network MIDI under the per-interface and firewall rules, labeled unauthenticated | Each is off after a factory reset, and reachable only on the interfaces chosen; mDNS carries only name and profile |
| 6. Robustness | IRQ steering on reliability-level products; limits tuned | On the target board, a Performance plays through a LAN flood at line rate and through junk at every open port with zero missed deadlines, and the device's own UI stays responsive |
| 7. Keys and updates | File ownership and Landlock exclusions for keys; root locked in product images; outbound rules per user; RFC 0035's tracking covering the network packages | A plugin worker and an app cannot read the TLS key, the signing key or write the trust store; the console asks for a login in a product image; a worker cannot connect out |

Step 1 needs nothing else and unblocks RFC 0031's step 2 on Linux. Steps 3 and 4 are RFC 0026's
step 1; step 4's grants are RFC 0026's step 2 and share code with RFC 0022's step 4. Network
control is offered on a device only after steps 2, 3, 4 and 6.

## Open questions

Decided by the owner, 2026-11-07: **OSC is configurable in Settings.** Off by default; when turned on,
the user chooses per interface between OSC only inside paired connections and the labeled
unauthenticated exception (a source allow-list, Play methods only). RFC 0003's bridge follows it.

- **The OSC bridge:** amend RFC 0026's "no unauthenticated network control" to allow it as a
  labeled exception (recommended), drop the UDP bridge, or carry OSC only inside the paired
  connection.
- **Link on venue Wi-Fi:** off per interface is proposed; is a follow-only mode wanted, and does
  Link allow one?
- **Should the engine verify each client's proof itself,** so a compromised gateway gains no
  grants? It costs moving key checks (and for mutual TLS, the TLS session's binding) across the
  process boundary.
- **Paired devices in a setlist:** a fifth permission ("Ensemble"), or Play plus named methods?
- **System over the network:** allowed by default for the owner's laptop, or off unless the
  owner enables it per device, with code installs always confirmed on the device's screen?
- **Mbed TLS 4.1 versus the 3.6 Buildroot packages:** carry our own package, or wait for
  Buildroot; and whether libcurl and libwebsockets build against 4.x.
- **mutual TLS or a signed challenge** for native clients.
- **Network MIDI:** RTP-MIDI is not designed in any RFC; its session protocol's authentication,
  if any, needs checking before it is offered.
- **Time:** whether NTS (authenticated network time) is worth carrying, against the last-known
  time rule alone.
- **The device's own access point** (a pedal that makes its own Wi-Fi network for a phone): its
  password, and whether it changes the defaults here.

## Ruled out

- **Any listener on by default,** including discovery.
- **Plain HTTP for the web UI or the control API,** and security built inside a page served over
  it.
- **A public certificate per device** under a project domain: it needs the internet and a
  service the project runs, and public certificates' shrinking lifetimes would lapse on offline
  instruments.
- **Bearer tokens in cookies** for browsers: ambient authority that another site can ride on.
- **The engine terminating TLS or parsing HTTP** on its control thread.
- **Port forwarding, UPnP, or reaching the device from the internet,** which needs its own RFC
  (RFC 0026).
- **Relying on the firewall alone,** or on binding alone: each service is closed by both.
