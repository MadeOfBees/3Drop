# Configuration and pairing

## Host setup

The front page's [What you need](../README.md#what-you-need) lists the commands every PC needs. The tools also use these where they are present:

- On macOS, the system's own `iconv`, which composes names (Unicode NFC), and `sips`, which adds a PNG of each screenshot.
- `nc`, where present, for some exchanges. Bash's own `/dev/tcp` carries the rest, and all of them on a PC without `nc` (Git Bash has none).

**Tested on:**

| PC | how it is tested |
|---|---|
| Linux | CI, on Ubuntu 24.04 |
| macOS | a Mac, on its own bash 3.2: 15 of the 16 shell tests pass, and testTree still fails there |
| Windows, under Git Bash | a PC |
| WSL 2 | the full test suite, on Ubuntu 24.04 |

- **WSL:** clone in the Linux home, as [Clone the tools](../README.md#3-clone-the-tools) says. Files on `/mnt/c` stay usable as inputs. Building needs Docker Desktop's integration for that distribution ([Docker's WSL page](https://docs.docker.com/desktop/features/wsl/)). WSL 2's default network keeps `discover` from finding the console, so give its address once (see [The console's address](#the-consoles-address)).
- **Windows, under Git Bash:** a clone gets LF line endings whatever Git's `core.autocrlf` says. Git Bash has no `nc` and does not tell file owners apart (see [Big files](file-transfer.md#big-files)). A `get` skips names Windows cannot make, such as `CON` (see [Folder copies](file-transfer.md#folder-copies)).
- **macOS:** the tools run on the system's own bash 3.2 and use its `iconv` and `sips`. On macOS 15 or later, a terminal app other than Apple's Terminal, such as iTerm2, needs macOS's permission to reach devices on the local network. If that permission was refused, commands fail with `no reply from <ip>:<port>` though the console is fine: turn the app on under System Settings, Privacy & Security, Local Network.
- **Linux:** a firewall can drop the answer to `discover`'s probe. Give the console's address once instead (see [The console's address](#the-consoles-address)).

## The key

3Drop generates the key on first run ([Launch 3Drop](../README.md#2-launch-3drop)). Every tool reads it from `THREEDROP_KEY` ([Pair](../README.md#4-pair-put-the-key-on-your-pc)). Keep it secret (see [Safety](safety.md#safety)).

- **Your own key:** keeping the generated one is best. A key you choose is a password anyone who captures one request can test guesses against offline. Make it long and random, at least the generated one's 60 bits. Write it into `key.txt` (1–128 characters, dashes ignored), then relaunch 3Drop.
- **A new key:** hold **UP+SELECT+B** on the idle screen, then press **A**. Every PC then needs the new key. Nothing is served while it asks, so after 30 s with no answer it gives up, as **B** does.
- **No key:** if 3Drop cannot read or create `key.txt` (for example on a read-only SD card), the screen says so and 3Drop accepts no connection. Fix `key.txt`, then relaunch 3Drop, or make a new key with **UP+SELECT+B**, as above.
- **One key, two consoles:** to the tools, the key is the console. Two consoles that hold one key answer to the same name and share one address book. At one port they share the folder-copy state and manifests too. No tool tells them apart, however you address them. Keep each console's own generated key, and give the tools the one you mean, with `THREEDROP_KEY` exported per command or a checkout per console.
- **Reinstall and the card:** the key lives in `sdmc:/3ds/3drop/key.txt`, outside the title, so an install, an update or a reinstall keeps it. On a blank card, 3Drop makes a new key. A copied card copies the key, and an older card or a backup brings back the key it held. None of that revokes a key. Change a leaked one with UP+SELECT+B, as above.

## The .env file

`.env` at the repo root (gitignored) holds the line `THREEDROP_KEY=<the-key>`. A tool reads `.env` only when `THREEDROP_KEY` is empty or unset. A non-empty exported key therefore wins, and `.env` is then not read at all. `.env` may set the variables below too, but some are read before it, so export those: `THREEDROP_BRIDGE_PORT`, `THREEDROP_PIECE`, `XDG_CACHE_HOME`, `THREEDROP_FIXTURE_DIR` and `BENCH_RESULTS`.

The tools run `.env` as shell code (see [Safety](safety.md#safety)). They first check every line, so a key is never run as a command:

- A line is blank, a comment, or one `NAME=value` (`export` before it is fine), with at most a comment after it, set off by a blank.
- A value is one word with no blank, quote, backslash, backquote, bracket, brace, parenthesis or `;&|<>`. A value may also be one quoted string. In `'…'` any characters are allowed. In `"…"` none of a backslash, backquote, bracket, brace or parenthesis is allowed.
- `$NAME` expands in a value. `${…}` and `$[…]` are refused outside `'…'`, where they are plain text, because their errors would print the value they name.
- A line that breaks a rule stops the tool before any line runs (exit 2), with `.env line <n> is not NAME=value`. The CR at the end of a line a Windows editor wrote is ignored, and a UTF-8 byte-order mark before the first line is ignored too.
- A `.env` in UTF-16, as Windows PowerShell 5.1's `>` writes it, stops the tool too, with `.env is UTF-16`. Save it as UTF-8.

## Environment variables

| variable | default | meaning |
|---|---|---|
| `THREEDROP_KEY` | no default | the key 3Drop shows on screen. Every tool needs it |
| `THREEDROP_PORT` | 1337 | the port the tools connect to, unless an address names its own (`ip:1337`), and `discover` probes. 3Drop always listens and answers on 1337 |
| `THREEDROP_TIMEOUT` | 15 | seconds an exchange waits for its reply. An uninstall or a tree `delete` waits 120. `discover` and a relaunch's knock wait 3 at each try, and the knock is the signed `stats` a tool repeats while 3Drop relaunches. A set value replaces them all |
| `THREEDROP_NAME` | `3drop-<tag>.local` | the name `discover` looks up. It can be another name your network gives the console, or empty for none. Empty saves the lookup's few seconds on a PC that cannot resolve `.local` names |
| `THREEDROP_PROBE` | 255.255.255.255 | where `discover`'s probe goes. It can be your network's broadcast address (`192.168.1.255`) when a VPN takes the default one elsewhere, or empty for none |
| `THREEDROP_COMPRESSION` | `none` | `auto` sends some bodies of one piece or less in gzip (see [Compression](file-transfer.md#compression)) |
| `THREEDROP_PIECE` | 33554432 (32 MiB); 8388608 under `bridge.sh` | bytes in a piece of a big file's `get` or `put`. It must be a multiple of 1048576, and 33554432 at most |
| `THREEDROP_BRIDGE_PORT` | 2121 | the port `bridge.sh` serves on, at 127.0.0.1. `0` takes any free port, which it prints |
| `THREEDROP_GHOST_FLOOR` | 800000 | bytes/s. `drop.sh` cancels an install and sends it again, once, when its average rate and two 8388608-byte segments in a row both run slower than this. That applies between `THREEDROP_GHOST_GRACE` and `THREEDROP_GHOST_MAX_ELAPSED` seconds into its send. Such an install is a ghost (see [Known limitations](troubleshooting.md#known-limitations)). `0` turns the check off, for a slow Wi-Fi link. When set in `.env` or exported, this floor also stops a `device.sh put` or `sync.sh` that slow, as a ghost (42) |
| `THREEDROP_GHOST_GRACE` | 45 | seconds into an install's send before the ghost floor applies |
| `THREEDROP_GHOST_MAX_ELAPSED` | 150 | seconds into an install's send after which the ghost floor no longer applies |
| `THREEDROP_GHOST_RETRY` | 1 | `0` makes `drop.sh` exit 42 on a ghost the console canceled, instead of sending the install again |
| `THREEDROP_DEPLOY_WAIT` | 60 | seconds `deploy.sh` waits for the new build to answer after its install |
| `XDG_CACHE_HOME` | `~/.cache` | where `3drop/` keeps remembered addresses, folder-copy state and the bench arms ledger (see [Benchmarking](benchmarking.md#benchmarking)) |

## The console's address

- Every `<ip>` is the address on the console's top screen, typed or pasted whole as the screen shows it (`192.168.1.20:1337`). Reserve that address in your router (a DHCP reservation) so it never moves.
- Each address your console signed a reply from is remembered under `~/.cache/3drop`, per key. The file is named by a tag derived from the key, never the key itself. The 8 most recent are kept, newest first.
- `discover`, and `bench.sh` given no address, try these in order:
  - each remembered address
  - the console's name, `3drop-<tag>.local`, with the tag 8 hex digits derived from the key. The PC's own resolver does this lookup, and it reaches mDNS on Windows, macOS and most desktop Linux.
  - a probe broadcast on UDP 1337, which 3Drop answers with its address (a firewalled Linux drops the answer)
- **Under WSL 2's default network,** the probe's broadcast stays in the VM, and `.local` needs mDNS set up in the distribution. Give the console's address once (`tools/device.sh stats <ip>`), and the tools then remember it. Windows 11 22H2 or later can set `networkingMode=mirrored`, which Microsoft documents as reaching the LAN directly, multicast included. There a `.local` name needs WSL 2.3.17 or later and mDNS. Whether the probe's broadcast arrives is untested, so give the address there too.
- The tools prove each address with one signed `stats` before using it, so a console that moved, or another key's 3Drop, is never taken for yours.
- When none answers, the tools say why for each address and way, then point at the screen.

## Updating 3Drop and the tools

**The tools and 3Drop update together.** Keep the clone on the release 3Drop runs. A release that changes the wire protocol works only with tools that speak it, and the tools say which side to update. A tool's `--version` and 3Drop's top screen name the two releases.

- **3Drop:** in 3Drop, press **Y** and scan the update QR, the front page's code of the newest `3drop.cia` (see [Install 3Drop on the 3DS](../README.md#1-install-3drop-on-the-3ds)). FBI's Remote Install, Universal Updater (UU) and `drop.sh --self` also work (see [Installing with drop.sh](../README.md#installing-with-dropsh)). UU offers updates only for a copy it installed.
- **Then the tools:** move the clone to the release 3Drop's top screen names, with `git fetch --tags && git checkout <tag>`.
- The tools check the two releases at every exchange. With a 3Drop of another protocol, only `stats` and a self-update still work. No other change runs on it, and a read's reply is refused as it comes. `grammar` and `unsupported` say the two may come from different releases (see [Releases](troubleshooting.md#releases)).
- A QR of the release you already run, or of an older one, is refused as not newer, and nothing changes. To go back to an older release, install it with FBI.
- To hear of new releases, watch this repo on GitHub (Watch, then Custom, then Releases).
- UU and FBI delete-install, so an update always applies. The key survives it, because `sdmc:/3ds/3drop/key.txt` lives outside the title.
- Each release re-publishes the store's manifest (`3drop.unistore`) with:
  - a higher `revision`, because UU keeps a fetched store only when that rises
  - a newer `last_updated`, because UU offers the update when that sorts after the one it recorded installing, so only for a copy UU itself installed
- To install a build of this tree, run `tools/deploy.sh <ip>`. It builds, packages and self-updates the console (see [Building](building.md#building)).

## Files and local state

### On the SD card

`sdmc:/3ds/3drop/` holds these:

- `key.txt` (the key)
- `key.cache` (the key again, beside its derived form)
- `3drop.log` (the event log)

At 1048576 bytes the log becomes `3drop.old.log`, replacing the last, so the two stay near 2.1 MB. Either log can be deleted. A reply the network cut is logged only for a change, so replays of a captured read, or junk, add nothing.

A `put` to `<path>` uses two names beside it, which are 3Drop's own. `<path>.3drop.tmp` holds its whole pieces (see [Big files](file-transfer.md#big-files)). `<path>.3drop.bak` holds the old file between the two renames that replace it (see [Known limitations](troubleshooting.md#known-limitations)).

A QR install keeps its certificate list in `sdmc:/config/ssl/cacert.pem`, which Universal Updater uses too (see [Install from a QR code](../README.md#install-from-a-qr-code)).

### On the PC

- **`.env`** at the repo root (gitignored) holds `THREEDROP_KEY` and any variables of [Environment variables](#environment-variables). The tools run it as shell code (see [The .env file](#the-env-file) and [Safety](safety.md#safety)).
- **`~/.cache/3drop`** (`$XDG_CACHE_HOME/3drop` when that is set) holds remembered addresses ([The console's address](#the-consoles-address)), folder-copy state ([Folder copies](file-transfer.md#folder-copies)) and the bench arms ledger, `arms.tsv` ([Benchmarking](benchmarking.md#benchmarking)). `THREEDROP_ARMS` can name another ledger instead.
- **`<local>.3drop-part` and `<local>.3drop-meta`** hold a stopped `get`'s whole pieces and its meta, beside its destination ([Big files](file-transfer.md#big-files)).
- **Benchmarking:** `bench-results.tsv` is in the current folder (`BENCH_RESULTS`), and fixture CIAs are in `buildFixture/` (`THREEDROP_FIXTURE_DIR`).
