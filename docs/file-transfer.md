# File-transfer reference

What `device.sh get` and `put` do with big files, pipes, compression, folders and patterns. The commands are indexed under [Files on the SD card](commands.md#files-on-the-sd-card).

## Big files

- A piece is 33554432 bytes (32 MiB), or a smaller size set with `THREEDROP_PIECE`. A file over one piece is sent or received in signed pieces of that size, each its own exchange. A piece cut short or damaged is sent again, three tries in all. A piece that went quiet goes again only if the console then answers a `stats`. Otherwise the run stops (124) with what came kept.
- A run that stops keeps its whole pieces. A `get` keeps them in `<local>.3drop-part` (beside `<local>.3drop-meta`), and a `put` keeps them in the console's `<path>.3drop.tmp`. The same command again resumes after the last whole piece.
- A `get` resumes only a part begun under the same key and port. It also needs the file's size and modification time to be unchanged (FAT, the card's file system, keeps times to 2 s). The last 1048576 bytes of the part must still match the file too. Otherwise it starts over.
- Two consoles that share a key and port, with a file of one size and time, are not told apart. Give each console its own key.
- The console's temp file stays until a put to that path finishes it or starts again. It also goes when you delete it, or when a piece is stuck on the card for ten minutes. After 3Drop has relaunched, or after a delete or rename has run, a resume first reads the temp file back, at about 11 MB/s.
- Every piece of a `put` but its last gives the file's size as `-`, so a `put` need not know it. The last piece names it, and only then does the file appear.
- Before starting, a `get` refuses to use a link as `<local>`, its part or its meta. It refuses another user's file there too, where the PC tells owners (Git Bash does not). It makes its part and meta only where nothing is. A screenshot's files go the same way.
- **Get into a folder only you can write to.** A `get` reopens its part and meta by name between pieces. That lets another local user who can write to the destination folder swap in a link or FIFO (a named pipe). A mount that maps owners (sshfs or NFS can) may show your own earlier file as another user's. That file is then refused too, so get into a folder on the PC's own disk.
- **FAT32's limit.** A file over 4294967295 bytes, FAT32's largest, is refused before anything goes, and stdin past that stops there.

## Pipes

- **`get <path> -`** writes the file to stdout in the same pieces. Each piece goes out only once it checks out, so a reader never sees a byte that did not. Nothing is kept, and a file that changes on the console as it comes stops the transfer (1). `from <n>` starts at byte n, where a cut transfer stopped. A reader that stops early ends the `get` with status 141, as it does any writer to a closed pipe.
- **`put - <path>`** reads stdin a piece at a time. Stdin of one piece or less, an empty stdin too, goes as a `put` of that file. A save's or extdata's file (extdata is the extra data some games keep), or one `--system` takes, goes only that way, as one piece or less. Run again on the same stdin, it reads past the whole pieces the console keeps and goes on. On other bytes it stops (1), and deleting `<path>.3drop.tmp` starts it over.

## Compression

Under `THREEDROP_COMPRESSION=auto`, some bodies go in gzip. The console inflates them as it writes, or deflates as it reads. The digest is always the file's own bytes.

- A `put` goes in gzip only when all of these hold:
  - the file is one piece or less (see [Big files](#big-files))
  - the file is 1024 bytes or more
  - its local name does not end in an extension that is already compressed, in any letter case (the list is `GZIP_SKIP_EXTS` in `tools/wire.sh`)
  - the gzip is smaller than the file
- A `put -` of a piece or less goes by the console name's extension.
- A `get` into a file of one piece or less asks for gzip. The console answers in gzip by the same name and size rules, but without checking that the result is smaller, so random bytes come back a little larger. The console deflates at level 1, and its CPU is the limit. Text comes and goes faster, but bytes that do not compress come several times slower. Leave `auto` off for data that is already compressed under a name the list does not know.
- **Never in gzip:**
  - a file over one piece (every piece goes raw)
  - a `put` into a save or extdata
  - `--system`
  - an install
  - `get -`
  - a `get` that resumes a kept part
- A gzip `put` goes whole. One cut short leaves nothing to resume, and sending it again sends the whole compressed body.
- Without `gzip` on the PC, a run says so once and sends everything uncompressed.
- The console's refusal `gzip` means a body was damaged or did not inflate to exactly the size it declared. Nothing is written. With result `d963f811` the console had no memory for gzip. Send the file again, or send it without compression. A `get` that finds no memory for gzip comes uncompressed.

## Folder copies

- `get <path> <local-dir> r` and `put <local-dir> <path> r` copy a tree one way, adding and updating only. Nothing is deleted on either side. Each file goes as a `get` or `put` of it does, and a big one goes in pieces (see [Big files](#big-files)).
- **Unchanged files are skipped.** A `put` skips a file whose SHA-256 is the one the last `put` of the tree committed. The card must still hold it at the size and modification time it had then. A `get` skips a file whose size and modification time on the console have not changed since the last `get` into that folder landed it. The folder must still hold it as it landed, by SHA-256. The PC's own times are never read or set. A `put` judges a PC file it would skip by its bytes as the copy starts, so a file that changes during the copy goes on the next run. The card's times are read once the files went, so another sender's rewrite at the same size during the `put`, or within FAT's 2 s after it, goes unseen.
- **Other files are not replaced without `--overwrite`.** Neither copy replaces a file on the other side that the last copy did not leave as it is. Such a file changed after that copy, or was there before it. The copy says so (`skipped '<name>': …; --overwrite replaces it`) and exits 1 once the rest went. Only `--overwrite` (`get <ip> --overwrite …`, `put <ip> --overwrite …`) replaces such a file. On the card, a name in another ASCII case is the same file to FAT, so it counts as a file no `put` from here left. Two names here that differ only in ASCII case (a case-sensitive PC holds both) are one file on the card. A `put` sends the first in byte order, skips the second, and says so.
- **Failures.** In each case below, the same command again goes on from there.
  - A file that cannot go (the console refuses it, or one side cannot make or read it) is said and skipped. The rest go, and the copy exits 1.
  - If a file gets no answer (4, 124) but the console then answers a `stats`, the file is said and left. In that case the rest go, and the copy exits 4.
  - The copy stops at once only on what every later file would meet too. If the console is busy or gone (3, 4, 124), it stops with that status. If the console is stuck in the system, its card is full, or the PC has no room for the file, it stops with 1.
- **Saves and extdata.** A save's or extdata's tree (`save:<title-id>/`, `extdata:<extdata-id>/`) goes the same way. Each file goes on its own, and a save commits after each one, so a copy cut short leaves the save or extdata partly old and partly new. An extdata file already on the card takes only a file of its own size (see [Known limitations](troubleshooting.md#known-limitations)). A save's or extdata's listing has no modification times, so a `get` of it fetches every file. A `put` reads a card file's bytes before replacing it, as it does for any file a `put` cut short never listed after.
- What each copy moved is kept under `~/.cache/3drop`, per console and per folder. The tools identify the console by its key and port, so its state is found at a new address too, as on another network. Another PC, or another folder, starts with a full copy. State these tools kept per address is not read, so the first copy of a folder it covers skips what is there, as above, until one `--overwrite` copies it again.
- Finder's `._*` and `.DS_Store`, a `get`'s own `.3drop-part` and `.3drop-meta`, and a `put`'s `.3drop.tmp` and `.3drop.bak` on the console never go either way. A link is never followed. A `put` says each link it leaves out. A `get` skips any file or folder whose place in the folder is a link, or lies through a link, says so and exits 1. A `put` also says each FIFO, socket or device it leaves out. It makes on the card each folder with no file to put under it, as a `get` makes it here.
- A `get` skips names the PC cannot make, says so and exits 1 once the rest landed. Those names hold a control character anywhere (`ls` shows it as `?`), or on Windows are `CON`, `NUL`, `COM1` and the other device names. It also skips the second of two files that the console keeps apart but the PC takes for one, by case or (on a Mac) composed and decomposed. That way the second never lands over the first. Two such folders become one. A listing that ended early exits 1 the same way. A `put` whose listing of the card ended early still goes, asking the console for each file it did not name.

## Patterns

- No FAT name holds `*` or `?`, so in the last part of the path of an `ls` or a folder `get` they are wildcards. `*` matches any characters and `?` matches one. Everything else matches itself (`[USA]` too), in either ASCII case, as FAT takes names.
- `ls` lists the matching names in the folder (`t` adds their times). If none match, it lists nothing.
- `get <path>/<pattern> <local-dir> r` takes what the pattern names, each folder with its tree, as a copy of the whole folder would. If the pattern names nothing, it exits 1.
- A `delete` never takes a pattern.
