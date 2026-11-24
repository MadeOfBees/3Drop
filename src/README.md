# The app's code

The C that runs on the 3DS, built into `3drop.cia` by the Makefile. A line a module: `name.c` is the code, and `name.h` is what the rest of the app may call. The host's tools are in `tools/`, and `tools/wire.sh` speaks the wire that `protocol.c` serves, so a change to one is a change to both. [Building](../docs/building.md#building) says how to build and test; [Device commands by task](../docs/commands.md#device-commands-by-task) lists what a host can ask.

## Where a request goes

`main.c` accepts a client and hands it to `cmdServe` (`cmd.c`). `protocol.c` reads the request line, `auth.c` checks its MAC (and a change's nonce), and the verb it names runs: `cmd.c` for titles and the console, `cmdFs.c` for files. A body that comes in goes through `transfer.c` into the ring (`ring.c`), whose worker writes it to the SD card (`sdXfer.c`) or into AM (`install.c`); a body that goes out takes the other way.

## Serving requests

- `main.c`: start-up, the loop that accepts a client and serves its request (`serveBusy` does it while a QR scan holds the thread: `stats` and `screenshot` at a screen, nothing during a download or install), the keys (X cleans up AM, Y scans a QR code, START leaves) and shutdown
- `app.c`: the app's shared state: the server's status, and the LED that goes with it
- `net.c`: the network's start-up, the listener and its buffers, and the reads and sends every request and reply uses
- `protocol.c`: reads a request line, runs the verb the table names once its MAC checks out (and, for a change, its nonce is spent), and signs each reply line
- `auth.c`: the pairing key (`key.txt`, stretched with PBKDF2), the MACs of requests and replies (HMAC-SHA512), and the nonces, each good once
- `cmd.c`: the verb table, and the verbs about titles and the console: `stats`, a CIA `put`, a title's `delete`, `run`, `seed`, `screenshot`
- `cmdFs.c`: the verbs about files, on the SD card, saves and extdata: `ls`, `get`, `hash`, `put`, `delete`, `mkdir`, `rename`
- `discover.c`: how a host finds the console without its address: its name `3drop-<tag>.local` over mDNS, and a probe on UDP port 1337

## Moving a body

- `transfer.c`: the pumps that move a body between the socket and the ring, or a sink or source of the caller's, under an idle and a total deadline, with START to cancel
- `ring.c`: the ring between the wire and AM or an SD file (a 24 MiB buffer, and up to 16 MiB more for an install bigger than that), and the worker thread that makes the slow calls, so the wire never waits on one
- `sdXfer.c`: SD-card get and put bodies, run on the ring's worker
- `install.c`: CIA installs through AM, by the route the request names: the import, its commit, or the cancel that says what is left
- `cia.c`: reads a CIA's header, ticket and TMD, so a CIA is checked before AM sees it
- `hash.c`: SHA-256 (the console's engine from 8 KiB to 1 MiB, software otherwise), and the wire's digest, the SHA-256 of a body's megabyte SHA-256s
- `stats.c`: the counters `stats` reports, filled in as a transfer runs

## Files and titles

- `fs.c`: the SD card, saves and extdata through FS
- `path.c`: how a request names a file or a save, and which paths it may not touch: the key, the console's own data, what it boots from
- `layeredFs.c`: what would crash the console in Luma's loader, refused first: a launch of a title with a `romfs` folder unless the user says it is a game, and an `exheader.bin` of a size the loader does not take, in a put or a rename
- `titles.c`: titles through AM: what is installed, which ones 3Drop may touch, and uninstall
- `screenshot.c`: a copy of both screens as a BMP, with the pairing key's rows blacked out
- `hbldr.c`: Luma's `hb:ldr`: which `.3dsx` the placeholder title loads, for `run`

## The Y key: installs from a QR code

- `qr.c`: the Y key: scan a code, show its links, fetch and install each CIA; the order the files below run in
- `scan.c`: the camera as a QR scanner
- `urls.c`: a scanned code's text as the URLs to fetch
- `download.c`: one libcurl transfer, on a thread of its own, into the ring
- `certs.c`: the trust policy: the certificate list on the card if it reads, else the one built into the app, and the fetch of a fresh one
- `caBundle.S`: `res/cacert.pem` as bytes of the app
- `quircLib.c` and `quirc/`: the QR decoder, upstream's files byte for byte (`quirc/UPSTREAM.txt`), built as one unit

## Screens, LED and system

- `ui.c`: the two screens: what each status and notice shows, a transfer's rate and time left, the pairing key, and the QR scan's views
- `led.c`: the notification LED, through the MCU: a color, steady or breathing
- `sysinfo.c`: the console's model and its battery and charger state, for `stats` and the discovery answer

## Shared

- `codec.c`: the wire's fields as text: hex, decimals, title ids and digests
- `knobs.h`: the bench build's knobs, which a release build compiles away
- `result.h`: the app's `Result` codes, and `Operation`: what a request did

## `bench/`

Compiled into the bench build only (`THREEDROP_TEST_VERBS=1 tools/build.sh`), never a release.

- `bench/bench.c`: the `bench` verb: knobs, state, per-megabyte rows and timelines, which `tools/bench/` reads
- `bench/capture.c`: screen captures for the screens `screenshot` cannot reach: a transfer, a CIA's download and install from a QR code, boot, a fatal error
- `bench/hw.c`: `bench hw`: a timed request's ring calls and wire gaps on one clock
- `bench/ledRead.c`: the LED's read-back, for `bench led` and the Testing Tool

The tests in `tools/tests/` compile these sources on the host against stubs of libctru, so the gate needs no console.
