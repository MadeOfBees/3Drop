# Safety

Discovery's name and probe answers are unsigned. Each address discovery finds is then proved with a signed `stats`. An install's signed request carries a digest of the CIA up to the end of its title metadata (TMD): the header, certificates, ticket and TMD. AM, the console's installer, checks every content against the TMD as it writes it.

- **The key runs code.** Whoever holds it can install an application's CIA, or `put` and `run` a 3dsx. That code can read the key, write NAND and drop tickets. The key alone can also roll back any application's save, since writing a save deletes its secure value. The refusals below and under [Known limitations](troubleshooting.md#known-limitations) stop a script's mistake, not a key holder. A new key does not undo what an old one installed.
- **Never delete a ticket you did not create.** A retail title's ticket is its license, and a wrong ticket delete can brick the console. `device.sh uninstall` keeps the ticket unless you pass `--ticket`, which 3Drop takes for applications only. 3Drop then acts on your word, because the console cannot tell a ticket you created from a license.
- **`--system` writes what the console boots from.**
  - Without it, `/boot.firm`, `/luma/payloads`, `/luma/sysmodules` and the external FIRMs Luma boots in place of the console's own (`/luma/native.firm`, `twl.firm`, `agb.firm`, `safe.firm`, `sysupdater.firm`) are refused (`system`).
  - With it, one file directly in `/luma/payloads` or `/luma/sysmodules` takes a `put` or a `delete`, and `/boot.firm` a `put`; anything else stays refused.
  - A new `/boot.firm` is checked on the SD card before the old file is replaced. The check looks for `FIRM`, an ARM9 entry point, and each entry point in a section. Each section must be whole 512-byte sectors inside the file and match its SHA-256. Each load address must be a multiple of 4 and inside the memory boot9strap loads into (ARM9 RAM, VRAM, DSP/AXI WRAM, FCRAM). The file must be as long as its header and sections.
  - These are not all of boot9strap's checks, so a FIRM that passes them can still be one boot9strap will not boot. 3Drop does not check that a section stays off the buffer boot9strap reads the file into. Nor does it make the two checks boot9strap adds before the lockout: no section in FCRAM, and a FIRM of 3 MB at most. boot9strap makes those when the FIRM's header asks to boot before the lockout, and the console's protected registers say the lockout has not come yet. 3Drop cannot read those registers. The checks were compared with boot9strap 1.4's own source (its `firm.c` and `main.c`). Another release may check otherwise.
  - The replacement is two renames, and a power cut between them leaves no `/boot.firm` on the SD card. So the console takes one only while it is on its charger (`system` otherwise). The fix is a card reader: rename `/boot.firm.3drop.bak` back to `/boot.firm`.
- **The key is a secret.** On the PC it lives in the gitignored `.env`, which the tools run as shell code, so keep it, and the checkout, yours alone. On the console it lives in `sdmc:/3ds/3drop/key.txt` and `key.cache`, which any app or 3dsx you run can read. Never paste the key into a commit, a log or an issue. The file commands refuse the key files.

**On your network.** Requests are signed, not encrypted, so use 3Drop on a network you trust.

- Anyone on it sees every file, listing, save and CIA you move, though a screenshot blacks out the key. They can also replay a read they captured (`ls`, `get`, `hash`, `stats`), at any time, and get what that path holds then.
- A change whose reply never came is not sent again. If it never reached the console, a captured copy of it still works until your next change, or for up to 5 minutes. So does a captured change the console refused before acting.
- Anyone on it can keep 3Drop busy, since 3Drop has no backoff. An unsigned connection holds it up to 15 s at a time. A captured read holds it for minutes to hours, at the pace its reader sets (a `hash` for as long as the card takes to read its file). A wrong MAC turns the LED red, though your key is unchanged.
- **A bare `ERR` is unsigned.** `ERR auth` and `ERR malformed` need no key, so anything at that address can say them. They report a 3Drop holding another key, or one that cannot read the request, but prove neither. A remembered address that answers `ERR auth` is dropped from the address book. One that answers `ERR malformed` stays in it. The tools accept neither an address nor a command's result on such an answer. They wait for a signed reply.
- Anyone on your subnet can look up the console's name for its address, and probe it for its address and model, with no key. Anything there could answer in the console's place. An address counts as the console's only once a signed exchange with it succeeds. `discover` asks that of each address, a remembered one too, before it names it.
- The name is a tag derived from the key, the same on any network and on a copied card. It carries none of the key. But anyone on the subnet can test a guess at the key against it offline, so keep the generated key, and one key per console.
- **`run … stdio`** sends a 3dsx's output in plaintext: see [Titles](commands.md#titles).

## QR downloads and certificates

This section adds what [Install from a QR code](../README.md#install-from-a-qr-code) leaves out: the rest of the confirm screen's rules, and how a QR download checks what it fetches.

**The confirm screen:**

- Six rows show at once. When there are more, Up and Down scroll them.
- The whole code is refused if it holds more than sixteen links. It is also refused if a link is not http or https, or if a site cannot be read or is over 120 characters.

**Certificates.** Every https download is checked against the certificate list at `sdmc:/config/ssl/cacert.pem`: the site's certificate must chain to one on it and name the link's host.

- **A certificate for an IP address works.** A link to an address such as `https://192.168.1.5:8443/` passes when the site's certificate lists that address among its IP-address names (`subjectAltName=IP:192.168.1.5`, which is what mkcert and `openssl` make). With no alternative names at all, the address as the certificate's common name passes too. Mbed TLS, which does the checking, compares the link's host with a certificate's DNS names only, so 3Drop adds this one rule, for IPv4 addresses. A DNS name, a certificate for another address and an IPv6 link are checked as before.
- **Dates are not checked.** The console keeps its own clock, with no time zone and no network sync, and it is often set wrong, or set back on purpose for games, so a date check would stop good downloads. The cost: an expired certificate still passes, anywhere in the chain. 3Drop does not check revocation either, so a certificate whose key was stolen stays usable against 3Drop by someone who can also get between the console and the site, such as on the same Wi-Fi. The chain and the name are still checked.
- **The first time,** with no list on the card, 3Drop fetches one from curl.se. That fetch is checked against the certificates built into 3Drop: Mozilla's list, as curl.se publishes it.
- **A failed check** names the link (its number, site and path) and the cause.
- **A fresh list** is checked against the list on the card. If that list is too old to vouch for curl.se, or cannot be read, the certificates built into 3Drop check it instead. It replaces the card's list only if every certificate in it is whole, each `BEGIN` with its `END`, and Mbed TLS can read at least one of them. A certificate Mbed TLS cannot read is left out, as libcurl leaves it out too. A page with no certificates, a cut file or a fetch that fails its check leaves the old list.
- **No fetch of the list goes unchecked,** and **Y** never skips the list's own check. When the first fetch fails its check, the screen says "The certificate list failed its check." with the cause, and the link is not installed.
- **Without the console's SSL service,** https links are refused (`no-tls`), and so is an http link that the server sends on to https. Plain http links still install.

**More on a QR download:**

- **Redirects:** an https link is only sent on to https. An http link may be sent on to http or https, in any order, up to 8 redirects, and every https hop is checked.
- **The name lookup** cannot be cut short. **B** waits for it, and the download's 15 s connect timeout does not bound it.
