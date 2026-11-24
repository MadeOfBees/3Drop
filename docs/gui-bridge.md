# GUI file clients

```sh
tools/bridge.sh [ip]   # then connect FileZilla, WinSCP or Cyberduck to the address it prints
```

`tools/bridge.sh [ip]` serves the SD card to a GUI file client. It runs an FTP server on this PC, on 127.0.0.1 alone, until Ctrl-C. With no address it finds the console as `discover` does. It prints its address, user and password, as in `ftp://127.0.0.1:2121 user 3drop password 3f9a…`, and the password is new each run. FileZilla, WinSCP and Cyberduck read and write through it. Finder's Connect to Server can only read over FTP.

- **Set the client to Binary and one connection.** ASCII would rewrite line endings in saves and cheat files, so a transfer in it is refused. FileZilla's default, Auto, sends `.txt` and extensionless files in ASCII, so choose Binary under Transfer, Transfer type. The console takes one transfer at a time, so a second one is refused at once. In FileZilla, open Site Manager, then Transfer Settings, and set Limit number of simultaneous connections to 1.
- While a transfer runs:
  - A change of folder is taken without a check. The next listing there says if it is not a folder.
  - A listing is refused at once with 451, which no client takes for an empty folder.
  - A delete, a new folder or a rename is refused at once with 450.
  - A refused request can be asked again once the transfer ends.
- Each command is a `device.sh` one: listings, `get`, `put`, new folders, renames, and deletes of a file or an empty folder. A client empties a folder itself. Installs, saves, extdata (the extra data some games keep) and `--system` stay in the terminal. The bridge never sends `--system`.
- Transfers go in pieces of 8388608 bytes (8.4 MB), set by `THREEDROP_PIECE`. Each piece is passed on only once it checks out, so a client that waits 15 s for bytes does not time out at the console's rates.
- A download starts only with its first piece, so a refusal before that piece is the download's own answer. A download resumes where it stopped.
- **An upload resumes when the whole file is sent again.** Choose Overwrite, not Resume, which is refused. The console keeps an upload's whole pieces in `<name>.3drop.tmp`, which the listing shows.
- A client that sends ABOR or quits during an upload stops it, and nothing is published. The same happens when the bridge itself stops (Ctrl-C, or its terminal closed). A client that only closes the upload's connection ends the file there, as on any FTP server.
- A refusal comes back in the console's words, with one of these codes:
  - 450 where the same request can work later. FileZilla retries those, and a cut upload of other bytes in the way is deleted first.
  - 552 for a full card or a file past 4294967295 bytes. Either way, the upload's kept pieces are then deleted.
  - 550 for the rest.
  - 451 when the console did not answer.
  - 426 when the console stopped answering once the data had begun.
- A request `device.sh` cannot take is 501. A transfer stopped by ABOR is 426, then 226.
- Times are the console's clock, shown in this PC's zone. An uploaded file gets the console's time, since nothing sets one. A name that holds a control character lists with `?` in its place and cannot be fetched.
- Under WSL 2, a client on Windows should reach the bridge through WSL's forwarding of 127.0.0.1, but that path is untested. If the client cannot connect or a transfer hangs, run the bridge in Git Bash.
- **The bridge is for a single-user PC.** Its data connections carry no password, as with any FTP server (RFC 2577), so another local user could take one. There is no FTPS.
