# Troubleshooting and limitations

## What a message means

A tool says why it stopped on stderr, starting with its own name (`drop: …`, `device: …`). A refusal from the console shows the console's reply, with its one word (`not-newer`, `route`, …), then a sentence where that word needs one. Find the first words here.

### Reaching the console

| the tool says | what it means |
|---|---|
| `no reply from <ip>:<port>` | the connection was refused or cut, and no reply came. 3Drop is not open at that address, or the address is another device's. Check the address on 3Drop's top screen. |
| `timed out: no connection to <ip>:<port> in <n> s` | nothing took the connection: a wrong address, a console asleep or off the network, or a guest network that keeps its devices apart. |
| `timed out: <ip>:<port> went quiet for <n> s` | the console stopped answering partway. Where `nc` carried the exchange, the connection may never have opened. Check as for the line above. |
| `no good answer from <ip>:<port> in 3 tries: …` | a read was asked three times, and no answer verified. The lines before it, each ending `(try <n> of 3)`, say what each try met. Check the address on 3Drop's top screen, that 3Drop is open, and that the PC is on the console's network. `tools/device.sh discover` looks for the console. |
| `a benchmark run on this PC holds …`, `the console is held by a benchmark run on this PC: …` | a benchmark run on this PC holds the console, so nothing was sent to it (exit 3). Wait until the run ends, or stop it (see [Benchmarking](benchmarking.md#benchmarking)). |
| `the 3Drop at <ip>:<port> holds another key: …` | `THREEDROP_KEY` is not the key on that console's screen, or that address is not your console. The console's `ERR auth` behind it is unsigned, so it proves neither (see [Safety](safety.md#safety)). |

When you don't know the address, `tools/device.sh discover` looks for the console and says why each address and each way got no answer (see [The console's address](configuration.md#the-consoles-address)).

### Pairing

| the tool says | what it means |
|---|---|
| `set THREEDROP_KEY to the 3Drop key (…), or put the line THREEDROP_KEY=<key> in …/.env` | no key reached the tool, so nothing was sent (exit 2). See [Pair](../README.md#4-pair-put-the-key-on-your-pc). |
| `…/.env line <n> is not NAME=value: …` | that line of `.env` is not one assignment, so no line of it ran (exit 2). See [The .env file](configuration.md#the-env-file). |
| `…/.env is UTF-16, as Windows PowerShell 5.1's > writes it: save it as UTF-8` | no line of `.env` ran (exit 2). Open it in an editor and save it as UTF-8. |
| `could not derive the key: openssl needs enc -pbkdf2 -iter (1.1.1 or later, or LibreSSL)` | the PC's `openssl` is missing or too old. Install OpenSSL 1.1.1 or later, or LibreSSL. |

### Releases

| the tool says | what it means |
|---|---|
| `3Drop <release> speaks protocol <n>, newer than these tools' <m>, …` | the console runs a later release than your clone, so only `stats` and a self-update still work with it. Update the tools as the message says (`git fetch --tags && git checkout <release>`). |
| `3Drop … speaks protocol <n>, older than these tools' <m>, …` (or `a protocol from before epochs`) | the console runs an earlier release. Update 3Drop as the message says (in 3Drop, **Y** and the update QR, or `tools/drop.sh --self`). |
| `grammar`, `unsupported` | the console's 3Drop does not take a word of this request. 3Drop and these tools may come from different releases. |
| `3Drop speaks protocol <n>, these tools <m>: a self-update goes either way, …` | a warning, not a refusal. The self-update crosses releases, so check that the build is the one you meant. |

See [Updating 3Drop and the tools](configuration.md#updating-3drop-and-the-tools).

### Installs

| the console's word, or the tool's line | what it means |
|---|---|
| `not-newer` | the console holds this title at the same version or newer, and takes an update only above it. Raise the CIA's version (`drop.sh` with no intent flag bumps an application's). For 3Drop itself, `tools/deploy.sh` numbers its build past the installed one, and FBI installs an older release. |
| `exists` | on an install, the title is installed already, and `drop.sh` with no intent flag sends an update. On a seed, the console holds one for this title already, and never replaces it. |
| `exists-pending` | an unfinished import of the title is held. Uninstalling the title clears it (see [Recovery](#recovery)). |
| `route` | only 3Drop's own title goes as `--self`, and it goes no other way. |
| `unmanaged`, `self`, `not-application` | 3Drop does not take that title for that command (see [Known limitations](#known-limitations)). |
| `notfound` | on an install, the title, or its save data, is not on the console. An update, or a save or extdata `put`, needs it installed. |
| `metadata` | that is not a CIA 3Drop can read. Its header, ticket or TMD did not parse. |
| `writer`, `ring` | the console had no memory to start the install, or the transfer. Send it again. |
| `unknown ticket`, on an install | nothing was installed, but its ticket may remain (see [Known limitations](#known-limitations)). |
| any other `unknown` | whether it took effect is unknown. Look before sending it again. |
| `no-length`, on a QR install | the link's server did not say the CIA's size (`Content-Length`), so it cannot be installed. Use a link that downloads the CIA itself, from a server that does. |
| `the seed was not fetched (…)` | Nintendo's seed server did not answer, so no seed went to the console. It matters only for a seed-encrypted title, which will not boot until it has its seed. `device.sh seed <ip> <id> <seed-file>` takes it from a 16-byte file. |
| `ghost: the send rate fell below THREEDROP_GHOST_FLOOR, …`, then `drop: ghost detected, so the install is sent again, once` | the install ran slower than the floor, 800000 bytes/s unless set, so `drop.sh` stopped it and sends it once more. AM slowing down for the whole install does this, and so does a Wi-Fi link that slow. If every big install stops this way, try `THREEDROP_GHOST_FLOOR=0`, in `.env` or exported (see [Environment variables](configuration.md#environment-variables)). |

### Files

| the console's word, or the tool's line | what it means |
|---|---|
| `short`, `receive`, `timeout`, `send` | the link cut the body or the reply partway (exit 4). Send it again. |
| `digest` | the bytes that came are not the ones the PC hashed. The file changed as it went, or was damaged on the way. Send it again. |
| `space`, `write`, `open` | the card has no room, refused a write (full, locked or failing), or could not make the file in that folder. A `write` with `c8804471` says a folder on the file's path is missing and the console could not make it. A save or an extdata holds only as many folders as it was formatted for. |
| `size` | a file in a save or extdata, or a `/boot.firm`, goes whole in the console's memory, so it can be 25165824 bytes at most. An extdata file already there takes only a file of its own size. |
| `isdir` | that is a folder. A folder comes with `r` (`get <path> <local-dir> r`). |
| `path`, `reserved`, `protected`, `system`, `refused` | the console never takes that path in this form (see [Known limitations](#known-limitations) and [Safety](safety.md#safety)). |
| `layeredfs`, on a `run` | not launched. A `romfs` folder crashes the console in Luma's loader unless the title is a game built with Nintendo's SDK. For such a game, use `tools/device.sh run <ip> --game <title-id>` (`sync.sh --game` for a sync). For your own app, delete the folder: `tools/device.sh delete <ip> /luma/titles/<title-id>/romfs r`. |
| `layeredfs`, on a `put` or a `rename` | not written. An `exheader.bin` that is not 1024 or 2048 bytes crashes the console in Luma's loader (a folder moved onto a title's directory is checked for the one in it). No other LayeredFS check is made on a write, so a `romfs` folder put by hand goes through. Homebrew that has one crashes the console when started from HOME with game patching on. 3Drop checks only a `romfs` folder at launch and an `exheader.bin`'s size. Luma's loader also stops on a `code.bin` too big for the title, or a code patch that doesn't apply. |
| `layeredfs` and a Result other than `00000000` | the card would not say whether a title has a `romfs` folder, or how big an `exheader.bin` is, so nothing was launched or written. Check the card and try again. |
| `the console is off its charger: …` | a `/boot.firm` goes only while the console is on its charger (see [Safety](safety.md#safety)). |
| `the listing ended early (…)`, then `read`, `deadline`, `depth` or `name` | the card failed a read partway; the walk ran out of time; a folder too many levels down was named but not entered; or a name the console cannot send was left out. List a smaller part, or the deeper folder by its own path. |
| `stranded` | a transfer is still stuck in the system (see [Recovery](#recovery)). |
| `busy` | the console is busy with a QR scan or install (see [Install from a QR code](../README.md#install-from-a-qr-code)). At a QR screen it answers `stats` and `screenshot`, and refuses everything else. During a download or install it answers nothing. Finish or cancel the screen at the console, then send the command again. |

## Exit status

| status | meaning |
|---|---|
| 0 | the tool did what was asked; `--help` and `--version` exit 0 too |
| 1 | the console refused, or the tool itself did (as for a file that is no CIA) |
| 2 | a call the tool cannot take, or no key to sign it with (see [Pairing](#pairing)) |
| 3 | a benchmark run on this PC holds the console (see [Benchmarking](benchmarking.md#benchmarking)). The exchange it stopped sent nothing, though earlier items of a batch, or files of a folder copy, may have gone |
| 4 | no answer came, or none that verifies, or the console says the transport cut a body (`short`, `receive`, `timeout`, `send`). The same request can go again |
| 124 | an exchange went quiet. Either no reply came within `THREEDROP_TIMEOUT` (see [Environment variables](configuration.md#environment-variables)), or nothing moved in a listing or a body for 240 s. That limit is 660 s on a `get`, an install or a `put` to the SD card, and more on a big file's `put` pieces, which the console may read back first |
| 42 | a send stopped under `THREEDROP_GHOST_FLOOR`, a ghost. `drop.sh` exits with it when it did not send the install again (a second ghost, or any under `THREEDROP_GHOST_RETRY=0`). A `device.sh put` or `sync.sh` exits with it when the floor is set |
| 141 | a `get … -` whose reader stopped early (see [Pipes](file-transfer.md#pipes)) |

stdout carries only what a tool produced: a reply, a listing, its report. A refusal, an error or a warning goes to stderr, named by its tool (`device: …`), with a sentence where the console's word needs one.

## Recovery

An unfinished import is cleared by uninstalling its title. **X** on the idle screen runs AM's cleanup of imports that finished but never committed. `Cleanup done`, or `Cleanup failed` with AM's code (in the log too), shows for a second, then the status screen comes back. "Stuck in the system" means the last transfer is waiting on a system call. It clears itself when the call returns. Until then, no transfer starts and START does nothing.

## Known limitations

Numbers are from an Old 3DS on home Wi-Fi.

- **Small installs are slow to start.** AM holds an install's first write while it prepares, and holds it longer the more titles are installed. A 1 MB CIA takes about 7–12 s on a console with 205 titles installed, and about 19 s past 210.
- **Downloads are slower than installs.** `get` runs at about 0.8 MB/s.
- **A rare install runs about 2.5 times slower**, because AM slows down for the whole install. Such an install is a ghost. `drop.sh` cancels and resends, once, an install that starts that slowly (see `THREEDROP_GHOST_FLOOR` under [Environment variables](configuration.md#environment-variables)).
- **Installs take applications, their updates and DLC only.** Demos, Download Play children, DSiWare and system titles are refused (`unmanaged`). So is a DLC CIA that leaves out contents its TMD lists (`metadata`). Every command that names a title answers in the same words: `unmanaged`, `self` for 3Drop's own title, `not-application` for a patch or DLC where only an application goes (`run`, `seed`, `--ticket`, a save, `/luma/titles`), and `notfound` for one not installed. Writes under `/luma/titles` and `/luma/plugins` are the exception: they check only the title's category. A CIA sent by the wrong intent is `route` (3Drop's own goes as `--self`, and nothing else does). Reading a save or extdata changes nothing, so it takes no title check and answers what the SD card holds.
- **Writing into a save or extdata is tested on one console and one title.** A `put` there replaces the file, so back the save up first. In a save, 3Drop deletes its secure value before it commits, so a game with anti-rollback does not take the new file for an old save. If the value will not go, nothing is committed (`secure-value`). Extdata has no commit. Its file is written in place, so a write that fails partway leaves it part old, part new, and the tool says the outcome is unknown. An extdata file cannot change size. One already there takes only a file of exactly its size (`size` otherwise). A missing one is made at the new file's size. In an extdata listing every file shows 0 bytes, because the console's file system gives no size there. A `get` still fetches the whole file.
- **`run <path.3dsx>` is experimental.** It needs Luma3DS v12.0 or later. On older Luma, every `run <path.3dsx>` is refused, whatever Luma's settings. When the 3dsx exits, Luma's loader starts the Homebrew Menu, not 3Drop, so remote access ends until 3Drop is relaunched. For an installed title, see [Back to 3Drop when your app exits](../README.md#back-to-3drop-when-your-app-exits).
- **Paths are UTF-8.** A path holding anything outside printable ASCII travels as hex. On macOS it is composed (NFC) first, because macOS hands out decomposed names that FAT would keep as second spellings. So a decomposed name already on the card is reachable only from Linux or Windows. 3Drop refuses a path (`path`) that is not well-formed UTF-8, or that holds a C0 control character or DEL. It also refuses a path with a segment of dots beside non-ASCII characters and no other ASCII but spaces (`..` and a no-break space, or `.ポケモン`). `ls` shows a control character in a name as `?`.
- **Look-alikes count as protected names.** Probed on a console, its file system folds, drops and trims none of the look-alikes tried (`ſ`, Kelvin `K`, `İ`, `ı`, fullwidth letters, `ü`, zero-width characters, trailing blanks). So for a change, a top-level name counts as a protected one only when it has as many characters, with a non-ASCII character standing for any. `/音楽` and `/Música` take writes. But `/3dſ` is `/3ds` (`protected`), `/boot.fırm` and `/ＬＵＭＡ/payloads` are boot files (`system`), and `/Nıntendo 3DS` is the console's data (`refused`). Below the top level, a component with any non-ASCII character still counts as every protected name it stands in place of. So nothing is written, deleted or renamed in a non-ASCII folder directly in `/luma` (`system`). Nor is anything written, deleted or renamed at a non-ASCII `/3ds/X` itself, or below three components that could each be the key's, `/3ds/3drop/key…` (`protected`). Reads compare ASCII names only, so they stay open everywhere but the key files themselves. Deeper non-ASCII names take writes (`/3ds/Checkpoint/saves/<id> <title>/…`, `/JKSV/Saves/<title>/…`).
- **A listing stops at 16 folders down, or after 120 s,** and says so (`depth`, `deadline`). List a deeper folder by its own path, or a smaller part.
- **One client at a time.** A request waits while another is served. If a request holds 3Drop longer than the tools wait for a reply (15 s), a change signed before that request is refused unrun. So a change whose tool gave up meanwhile never lands late. A tool still waiting sends it again, once.
- **A `put` replaces a file by two renames:** the old file to `<path>.3drop.bak`, then the new one in. Power lost between them leaves `<path>` missing, the old file in `<path>.3drop.bak` and the new one in `<path>.3drop.tmp`. They stay so until the next `put` to that path, which puts the old one back first. The console cannot boot without `/boot.firm`, so for it see [Safety](safety.md#safety). Those two names are 3Drop's. Nothing is put, made or moved onto a name ending `.3drop.tmp` or `.3drop.bak` (`reserved`). A `put` of `<path>` takes both as its own. A `delete` or `rename` of `<path>` that succeeds deletes its `.3drop.bak`. A `delete` or a move away still works on either name.
- **The lid closed from HOME or the POWER menu sleeps the console.** 3Drop keeps Wi-Fi up only while it is the app on screen. Once the console sleeps, a transfer times out, and nothing reaches the console until the lid opens.
- **Untested: Wi-Fi lost with the lid closed may keep the console off the network until the lid opens.** 3Drop takes the system's lock that keeps Wi-Fi up as it starts listening. Once it holds the lock, it never takes it again. So the outcome rests on whether a Wi-Fi loss lets the lock go.
- **Rosalina's InputRedirection and debugger take the same lock as 3Drop.** One app at a time holds it. If one of them holds it when 3Drop starts, Wi-Fi stays up on theirs. While 3Drop waits for a command, it asks for the lock every 10 s, so it gets it soon after theirs ends. Untested: one turned on while 3Drop runs gets no lock of its own, so after 3Drop exits, closing the lid may take its Wi-Fi down. To get it one after 3Drop exits, turn both off, then turn back on the one you need.
- **Rosalina's InputRedirection can freeze the console, with or without 3Drop.** It is a Luma bug (Luma3DS issues [#935](https://github.com/LumaTeam/Luma3DS/issues/935), [#1852](https://github.com/LumaTeam/Luma3DS/issues/1852) and [#2250](https://github.com/LumaTeam/Luma3DS/issues/2250)). Leave it off unless you are using it.
- **Nothing checks the battery before a long install.** `stats self` shows it (`bat=`, `ac=`). Only a `/boot.firm` `put` needs the charger. A battery that runs out mid-install is a power cut.
- **An install the console abandons can leave its ticket behind.** A crash or power loss during a `new` install or `uninstall --ticket` can leave a ticket without a title. A `new` install can also leave its ticket when AM refuses the cancel (`unknown am-cancel`) or its deletion after cancellation (`unknown ticket`). A refused cancel may still commit: check HOME first. HOME does not show tickets, and 3Drop cannot uninstall a ticket alone. In FBI, use Tickets, the affected title ID, Delete Ticket. Delete only a ticket you created; Delete Unused Tickets can remove licenses for uninstalled games.
- **A `sync.sh` cut short launches nothing, but leaves the stub part new, part old** on the SD card, and the title launched from HOME runs with it: sync again first.

## Screen and LED

The top screen shows the console's address, 3Drop's release and build number, the pairing key while idle, and each transfer's progress, rate and time left.

- **On the idle screen,** **START** exits, **Y** scans a QR code (see [Install from a QR code](../README.md#install-from-a-qr-code)), **UP+SELECT+B** makes a new key (see [The key](configuration.md#the-key)), and **X** runs AM's cleanup (see [Recovery](#recovery)).
- **During a transfer,** **START** cancels it. On the install-failed screen it only dismisses the error, and while a transfer is stuck in the system it does nothing. HOME is disabled while an install is writing.
- **An install that did not commit** ends on a card that says how:
  - `Canceled`: this CIA was not installed.
  - `Canceled, ticket may remain` (see [Known limitations](#known-limitations)).
  - `Install unconfirmed`: AM could not say how it ended. Look in HOME before you send it again.
  - The refusal, with its code and what it means.

With the lid closed, 3Drop keeps Wi-Fi up and the notification LED shows the status:

| LED | status |
|---|---|
| purple, breathing | listening |
| blue, breathing | starting up, receiving, installing, moving a file, or stuck in the system |
| green, steady | the install committed |
| red, steady | install failed, bad MAC, no pairing key, or not listening (Wi-Fi dropped, port 1337 taken, or 3Drop could not start a system service, which `3drop.log` names) |

An install's color stays until the lid opens, the next transfer starts or a request with a wrong MAC comes. While the listener is down, red shows over it, and the install's color comes back with the listener. A dark LED with the lid closed means 3Drop is not running.
