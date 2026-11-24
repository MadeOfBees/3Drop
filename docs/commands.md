# Device commands by task

`tools/device.sh` with no arguments prints every command, and `--help` prints it on stdout. The wire grammar lives in `tools/wire.sh` (host) and `src/protocol.c`, `src/cmd.c` and `src/cmdFs.c` (console). Every `<ip>` takes the forms under [The console's address](configuration.md#the-consoles-address). A `<title-id>` is 16 hex digits, as `titles` lists them, and an `<extdata-id>` 8.

## Titles

| what you want | command |
|---|---|
| installed titles | `tools/device.sh titles <ip>` |
| launch an installed application | `tools/device.sh run <ip> <title-id>` |
| launch a game built with Nintendo's SDK that has a `romfs` folder (see [LayeredFS sync](../README.md#layeredfs-sync)) | `tools/device.sh run <ip> --game <title-id>` |
| give a seeded game the seed it needs to boot | `tools/device.sh seed <ip> <title-id> [seed-file]` (see [Seeds](../README.md#seeds)) |
| delete a title: an application with its save data, a patch (its game reverts to the base version) or a DLC | `tools/device.sh uninstall <ip> <title-id>` |
| run a 3dsx (experimental; remote access ends) | `nc -l 17491 &` for its output, then `tools/device.sh run <ip> /3ds/app.3dsx stdio` |

- **Tickets:** Never delete a ticket you did not create (see [Safety](safety.md#safety)).
- **A 3dsx** runs through Luma's loader. When it exits, Luma's loader starts the Homebrew Menu, not 3Drop (see [Known limitations](troubleshooting.md#known-limitations)). `run … stdio` sends the 3dsx's output, unsigned and in plaintext, to the address the request came from, and `nc -l 17491` prints whatever connects to it first. Under WSL 2's default network that address is Windows, not the VM, so a `nc` inside WSL receives nothing.

## The console

| what you want | command |
|---|---|
| the running build, SD space, AM's state, Wi-Fi, battery, model, firmware and lid | `tools/device.sh stats <ip> self` |
| relaunch 3Drop | `tools/device.sh restart <ip>` |
| find the console's address | `tools/device.sh discover [seconds]` prints `<ip> <port> <model>` for each address your console answers at, probing for the seconds (2; see [The console's address](configuration.md#the-consoles-address)) |
| screenshot both screens | `tools/device.sh screenshot <ip> <out-prefix>` |

In a `stats` reply, `-` means a value was not measured, and `0` is a measured zero. `stats self`'s `lin_free=` is the console's free linear memory, in bytes.

## Files on the SD card

| what you want | command |
|---|---|
| list | `tools/device.sh ls <ip> <path> [d\|t\|dt\|r\|rt]`: `d` shows the path itself, `r` the tree, `t` modification times |
| list / pull by a pattern | `tools/device.sh ls <ip> '/3ds/*.3dsx' [t]`, `get <ip> '/saves/*.sav' <local-dir> r`: `*` and `?` in the last step, in either ASCII case; never `delete` (see [Patterns](file-transfer.md#patterns)) |
| rename | `tools/device.sh rename <ip> <from> <to>` |
| delete | `tools/device.sh delete <ip> <path> [r]`: `r` deletes a tree |
| make a folder | `tools/device.sh mkdir <ip> <path>` |
| pull / push a file on the SD card | `tools/device.sh get <ip> <path> <local>` / `put <ip> <local> <path>` (see [Big files](file-transfer.md#big-files)) |
| check a file on the SD card against one here | `tools/device.sh hash <ip> <path> [local]` prints `u:<hex> <size>`, the console's digest of the file, read on the card with nothing sent; with a local file it exits 1 when they differ |
| pull a file to a pipe / push one from a pipe | `tools/device.sh get <ip> <path> - [from <n>] \| tar t` / `tar c dir \| tools/device.sh put <ip> - <path>` (see [Pipes](file-transfer.md#pipes)) |
| pull / push a folder, adding and updating only | `tools/device.sh get <ip> [--overwrite] <path> <local-dir> r` / `put <ip> [--overwrite] <local-dir> <path> r` (see [Folder copies](file-transfer.md#folder-copies)) |

## Saves and extdata

| what you want | command |
|---|---|
| list / pull / push a game's save or extdata | `tools/device.sh ls <ip> save:<title-id>/ r`, `get <ip> save:<title-id>/<file> <local>`, `put <ip> <local> save:<title-id>/<file>` (`extdata:<extdata-id>/…` alike) |

Writing into a save or extdata is tested on one console and one title, and a `put` there replaces the file: back the save up first. An extdata file keeps its size, so it takes only a file of that size (see [Known limitations](troubleshooting.md#known-limitations)). A tree of either copies as a folder does, with the differences under [Folder copies](file-transfer.md#folder-copies).

## Boot files

| what you want | command |
|---|---|
| update Luma (`/boot.firm`) or a payload or sysmodule | `tools/device.sh put <ip> --system <local> /boot.firm` (or `/luma/payloads/<file>`, `/luma/sysmodules/<file>`), `delete <ip> --system <path>` for the last two |

**`--system` writes what the console boots from.** `/boot.firm` takes only a `put`, of a FIRM the console checks on the SD card before it replaces the old one, and only while the console is on its charger. Which paths `--system` opens, the FIRM checks, and what a power cut during the replacement leaves: [Safety](safety.md#safety).
