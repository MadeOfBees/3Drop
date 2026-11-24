# Benchmarking

The bench kit times installs from this PC, to tell whether a change made 3Drop faster or slower. It needs:

- Docker running (the Testing Tool is built in the build image), with no container up while a block runs
- `perl` and `openssl`
- to build its fixtures, `bannertool` and `makerom`, and, for a size above 0 whose RomFS is not yet kept (see [A block's cost](#a-blocks-cost)), `3dstool`

It finds those three as `makeFixture.sh` does (`$BANNERTOOL`, `$MAKEROM` and `$THREEDSTOOL`, else your `PATH`, else `tools/bin/`). If anything it needs is missing, it refuses (exit 2) before the console is asked anything. Fixture CIAs are kept in `buildFixture/`, or in `THREEDROP_FIXTURE_DIR`.

Its terms:

- **rep:** one timed install of a Testing Tool CIA (the Testing Tool is 3Drop's test title, `tools/bench/testingTool.c`).
- **fixture:** the CIA a rep installs, the Testing Tool with `size` × 1048576 bytes of random RomFS (`tools/bench/makeFixture.sh`).
- **block:** the reps `bench.sh` runs under one label, then scores. `bench.sh`'s usage and messages call a block an "arm", but this page keeps that word for one side of an A/B.
- **arm:** one side of an A/B, a build or a knob setting.
- **arms ledger:** the record of each build `deploy.sh` installed on a console.
- **card floor:** a timed 16777216-byte SD write without AM, the console's installer.
- **drain ring:** the console's RAM buffer between Wi-Fi and AM or the card (a rep's `ring_cap`).

## Running a block

1. **A bench build:** `THREEDROP_TEST_VERBS=1 tools/deploy.sh <ip>` installs one, with the `bench` verb (knobs, per-MB rows, timelines, the card floor). It records the build in the arms ledger (`~/.cache/3drop/arms.tsv`, or `THREEDROP_ARMS`), as `deploy.sh` does every build. A release build can be measured too, without knobs. Its rows have no `mb`, `air` or card floor.
2. **A block:** `tools/bench/bench.sh <label> [reps] [size] [ip]` relaunches 3Drop, which resets every knob. The script then sets `BENCH_KNOBS` and installs `reps` Testing Tool CIAs (8 by default, 15 at most). Each has `size` × 1048576 bytes of random RomFS (500 by default, which makes a 528 MB CIA). Each install gets a fresh title id and is timed on this PC from `drop.sh`'s start to its end. That time is the rep's wall. After each rep, the script uninstalls that rep's title with its ticket. Each rep adds a row to `BENCH_RESULTS` (`bench-results.tsv` in the current folder) and prints a line. The block ends with its score. With no address it finds the console as `discover` does.
3. **The score again:** `tools/bench/seriesScore.sh <label> [results] [expected-reps]`.

- **Refusals,** before any install, for:
  - a label that already has rows, or that is empty or holds a control character
  - a results file of another column layout
  - reps outside 1–15, or a size that is not a whole number (both exit 2)
  - a PC running docker containers or a `tools/check.sh` gate, since the PC is the sender
  - a console off its charger
  - a build other than the last the arms ledger records for that console at that address (a console that moved address needs `deploy.sh` there first)
  - `BENCH_KNOBS` on a release build, or a knob the console did not apply or reads back otherwise
- The PC check and the charger check run again before each rep. A rep that fails one is `skipped:host` or `skipped:ac`, with no install.
- A block stops when the console's drain ring is not the shipped one, or a rep's own title stays installed (`reclaimTestTitles.sh <results> <ip> --delete` removes it).
- A quiet PC has no docker container running and no gate. A gate is `tools/check.sh` run as a command (`bash tools/check.sh`, `./tools/check.sh`). A command that only names it, as an editor or a grep does, is no gate. On Git Bash, whose `ps` lays out its columns differently from Linux's and macOS's, `bench.sh` sees a gate only while one of its containers runs, so don't start a gate during a block there.
- **Knobs:** `BENCH_KNOBS='rcvbuf=32768'` sets them for a block, `0` being the default, and each must read back as set. An install exercises two knobs:
  - `rcvbuf`, the receive window: 49152 by default, at most 65535.
  - `nodisc=1`: no answer to the name or the probe, with discovery's sockets closed and its multicast group left. This prices holding them open. Nothing is answered while a request runs either way.
- The other three knobs never act on an install, so a block that sets them tests nothing:
  - `sndbuf`, the send buffer: 65536 by default, at most 262144. It acts on sends, as `probe.sh tx` or a `get` makes.
  - `sendsize`, the send chunk: 65536, which is its ceiling too. It acts on sends too.
  - `nohash=1`: a `put` to the SD card skips its digest, which prices it. Such a put is unverified. This knob acts on a file `put` to the SD card.
- **Deadlines:** `BENCH_DEADLINE`, in whole seconds, ends a rep after that many seconds, by default 120 plus 1 per 250 KB of the CIA. It bounds each `probe.sh` sub too (`rx` and `tx` by default 240 plus 1 per 250 KB, the others past their own timeouts).
- **Rows:** every rep a block starts writes a row, even one that could not start. Such a rep is `skipped:fixture` (its fixture was not built), `skipped:lock` (another rep held the console) or `skipped:listing` (the console's listing of its title id could not be read). Each is counted, never ranked, never reclaimed. The block's last card floor is on its last rep's row (`card_after`), however that rep ended. A block with a rep that never started still exits 0, as the score does, so a script that needs every install to have run reads the score's `skipped` count. A change to `tools/` during the block, a drain ring that is not the shipped one, a row that cannot be written, and a title left installed stop the block.
- Walls are timed on perl's monotonic clock. On a perl without `CLOCK_MONOTONIC` they come from the wall clock instead. Then `bench.sh` says so before rep 1, and each rep's line ends `clock wall`. If a step of that clock leaves a rep's wall at 0 or below, the rep is `void:clock`.
- `bench.sh --help` prints its usage and exits 0. `bench.sh --prefix-only [results]` prints the uid prefix the next block would take from that results file.

## Outcomes

Each rep's row and line name how it ended:

- **`committed:ok`:** the install committed, and its `stats` vouch for it. It is ranked by its wall.
- **Failed:** `rejected:<word>` or `unknown:<word>` (the console's answer), `deadline` (`BENCH_DEADLINE` ran out) or `noreply` (`drop.sh` named no outcome). A failed rep ranks as the slowest, since its wall is a lower bound, not a result.
- **`void:<cause>`** is counted, never ranked. The causes are:
  - `ghost`: a ghost retry, so two installs in one wall. A link slower than `THREEDROP_GHOST_FLOOR` (800000 bytes/s by default) can set off the ghost check too, and then reps are retried or fail: set `THREEDROP_GHOST_FLOOR=0` for the block
  - `req`: another request reached the console between the install and the rep's reads
  - `stats`: no `stats` came
  - `bytes`: the `stats` are of another size
  - `am-writes`: the stats show no write to AM
  - `clock`: no clock gave the rep a time
- **`skipped:<cause>`** is counted, never ranked, with no install. The causes are:
  - `self`: its `stats self` named no build, so the code under test is unknown
  - `knobs`: no answer to a bare `bench`, so its knobs are unknown
  - `ac`: the console on battery
  - `host`: the PC not quiet
  - `exists` or `am-info`: its fresh title id was already taken
  - `fixture`, `lock` or `listing`: under Rows, above
  - the word the console refused its listing with

## An A/B

Give each block its own label and run them in ABBA order, `A1`, `B1`, `B2`, `A2`, from the same PC and the same checkout. Install each arm's build before its blocks. For a knob, keep one build and set `BENCH_KNOBS` for the B blocks alone. Compare adjacent blocks in pairs: A1 with B1, B2 with A2.

## The score

- **A line per rep** shows:
  - its outcome, wall and rate
  - `first_write_ms`, how long AM took over the install's first write, in which it allocates the title's clusters
  - how many MBs took 2 s or more, and the slowest (`slow_mb`, `worst_mb_ms`)
  - its stalls, gaps of 100 ms or more in the data arriving, the console's own waits for its card or AM aside
  - `air`, the rate of a 16777216-byte (16.8 MB) receive just after it, shown only when that receive moved all its bytes

  `after relaunch` marks rep 1, which follows the block's relaunch. `after card floor` marks a rep whose first write a card floor lengthened. Each card floor prints as a rate.
- **Then the block** shows:
  - `n`
  - the median wall and the rate at that wall (`FAILED` once half the reps or more failed; an even count averages its middle pair)
  - `p90` (by nearest rank, so the slowest of 9 reps or fewer) and the fastest
  - the device median, the median of the committed reps' own rates as the console timed them, without the PC's work (so a median over fewer reps than the median wall)
  - the spread of the committed reps' rates
  - `bad runs`, each failed or with a wall over 1.25 times the median
- **Counts:** `failed`, `void` and `skipped` (see [Outcomes](#outcomes)).
- **Warnings:** `MISSING` or `POOLED` says the block has fewer or more rows than the expected reps. `TICKET-GAP` says one of these:
  - a rep left a ticket
  - a ticket went that no rep brought
  - the gap moved and came back, which a net of zero would hide

  The gap is `am_tickets` less `am_titles`, read after the last rep's uninstall too.
- **Exit:** 1 when the block is `MISSING` or `POOLED`, its median is `FAILED`, or no rep ran and counted. Otherwise 0.

**What pools.** A label's rows score together only under one binding, and are refused otherwise. A binding is the build (`ver`), the PC's tools (`inst`), the knobs and capture. The tools' hash covers every `tools/*.sh` and `tools/bench/*.sh`, so editing any of them starts a new one. After any change, start a new label.

The sample below is the real output of `seriesScore.sh` for eight made-up rows of a 528579072-byte fixture. No console ran them:

```
r1    committed:ok       wall    336.10    1.5727 MB/s  first_write_ms   2037  slow_mb    2  worst_mb_ms   2190  stalls   1  air 1.6777  after relaunch
r2    committed:ok       wall    331.40    1.5950 MB/s  first_write_ms   2074  slow_mb    3  worst_mb_ms   2280  stalls   2  air 1.6777
r3    committed:ok       wall    338.90    1.5597 MB/s  first_write_ms   2111  slow_mb    1  worst_mb_ms   2370  stalls   3  air 1.6777
r4    committed:ok       wall    333.20    1.5864 MB/s  first_write_ms   2148  slow_mb    2  worst_mb_ms   2460  stalls   0  air 1.6777
r5    committed:ok       wall    347.60    1.5207 MB/s  first_write_ms   2185  slow_mb    3  worst_mb_ms   2550  stalls   1  air 1.6777
r6    committed:ok       wall    335.70    1.5746 MB/s  first_write_ms   2222  slow_mb    1  worst_mb_ms   2640  stalls   2  air 1.6777
r7    committed:ok       wall    341.30    1.5487 MB/s  first_write_ms   2259  slow_mb    2  worst_mb_ms   2730  stalls   3  air 1.6777
r8    committed:ok       wall    334.80    1.5788 MB/s  first_write_ms   2296  slow_mb    3  worst_mb_ms   2820  stalls   0  air 1.6777
A1 n=8  median 335.90 s (1.5736 MB/s)  p90 347.60 s (1.5207 MB/s)  fastest 331.40 s (1.5950 MB/s)  device median 1.6170 MB/s  spread 4.9%  bad runs 0  failed 0  void 0  skipped 0
ticket gap +0 over reps 1-7
```

## The columns

| column | what it holds |
|---|---|
| `series`, `rep` | the block's label, and the rep's number in it |
| `time` | when the row was written, by the PC's clock |
| `uid`, `title` | the fixture's unique id and title id, fresh for each rep |
| `size_bytes` | the fixture CIA's size |
| `wall_s`, `rate_mbs` | the wall on this PC, from `drop.sh`'s start to its end, and the size over it. The rate is `-` for a rep that neither committed nor went void |
| `outcome` | see [Outcomes](#outcomes) |
| `ver` | the build the rep ran under, from `stats self` |
| `inst` | the hash of the tools the rep ran under |
| `knobs` | the knobs the rep ran under, as the console reports them |
| `capture` | whether capture-on-change was on |
| `am_titles`, `am_tickets`, `free_bytes` | from `stats self` before the rep: the titles and tickets installed, and the SD card's free bytes |
| `air` | the 16.8 MB receive after the rep |
| `card_before`, `card_after` | the card floor before the block or after a bad rep, and after the last rep |
| `stats`, `mb` | the console's `stats` for the rep's install, and its per-MB rows (`-` when the console's last curve is another request's) |

## A block's cost

A default block, 8 reps at size 500, installs 8 fixtures of 528 MB. At 5.5–5.8 minutes an install, as on our Old 3DS over home Wi-Fi, that is about 45 minutes of installs. Around them come:

- a card floor and a relaunch before rep 1
- each rep's `stats`, `mb`, 16.8 MB receive and uninstall
- a card floor after any bad rep, and one after the last

The PC makes a size's RomFS once and keeps it beside the fixtures, by the size and the hash of `res/fixtureCFA.rsf`. Each rep then packages the Testing Tool again, at its own title id. The console needs room for one fixture at a time, since each rep uninstalls its own title.

## Running the Testing Tool by hand

`tools/bench/makeFixture.sh 0` builds it as `buildFixture/fixture0m.cia`, under unique id `ff430` (title `000400000ff43000`). A different id goes after the size, in hex from `300` to `fffff`. The script refuses 3Drop's own `3d372`. Install the CIA with `tools/drop.sh`. Then `tools/bench/probe.sh tool <ip> 000400000ff43000` runs it unattended and prints its evidence. The evidence's `build` line names the version that ran.

## Other tools

Each tool in `tools/bench/` prints its usage with no arguments:

- `probe.sh <sub> <ip>` is one `bench` exchange. The subs are:
  - `--smoke` checks a bench build fresh from `run self`: its state, which may hold later keys, memory, an rx, mb, a tx and an SD write, each by value
  - `state`
  - `mem` (heap and linear memory)
  - `led` (the LED, its last write and its longest gap, which the read resets)
  - `disc` (discovery's counters)
  - `fscalls` (the last change's file steps and its log line, each timed)
  - `rx` and `tx` (the receive and send rates with no card or AM in the way)
  - `file` (the SD write floor without AM)
  - `mb` and `hw` (the last transfer's per-MB rows and its timeline; for an install, `mb`'s `sd_kb` is AM's import position)
  - a knob's name, which reads or sets it
  - `tool <title-id>` (or `title:<id>`), which runs the Testing Tool unattended and prints its evidence, ending `reads=<good> of <all>` (with none good it fails). `evidence <file>` checks that evidence.

  Every sub but `tool` and `evidence` refuses a release build with `not a bench build: THREEDROP_TEST_VERBS=1 tools/deploy.sh <ip>`. `probe.sh` alone lists every sub with its reply's keys.
- `capture.sh on|off <ip>` or `capture.sh fetch <ip> <outdir>` handles capture-on-change, a screenshot at each change of the screen, to see what it showed during a transfer. `on` and `off` set it now and for every later boot. They fail when the card's arm file says otherwise, because the setting would then hold for this boot only. `fetch` downloads each screenshot (a PNG too where `sips` exists) and deletes it on the console. It refuses before it downloads anything if the folder already holds a file of any capture's name. Capture slows transfers, so rows taken with it on say `capture=1` and never pool with others.
- `reclaimTestTitles.sh <results> <ip> [--delete]` lists, or uninstalls with their tickets, the titles reps left. It takes no other word. It looks only at rows whose rep reached AM. It removes a title, with its ticket, only when the console's own size for it (`ls title:`) is within 1 MiB of its row's `size_bytes`. That marks the title as the fixture that row installed. Any other title at a bench title id (another client's install since, or one AM gives no size for) is listed as `untied`, with the `tools/device.sh uninstall <ip>:<port> --ticket <title>` command, and left. `--delete` then exits 1. It cannot tell its own fixture from another results file's fixture of the same size at the same uid, and says a removal it cannot confirm is `unconfirmed`.
- **Tickets:** the console drops a managed title's ticket whenever it holds one, and knows that a ticket existed before only during the install. A ticket that already existed at a uid the bench picked therefore goes with the bench's title, in each rep's own uninstall as in reclaim.

## Measuring rules

Rules this project learned:

- Every number names the code, the request and the knobs it was measured with. A rate is what one build did with one request at one time, never a ceiling.
- Read the card floor (a 16777216-byte SD write without AM) before blaming a rep on the code, because a slow card lengthens the first write. The card floor is taken before a block, after a bad rep and after the last (`card_before`, `card_after`).
- A build swap or a relaunch makes the next rep slow. Never pool across a swap, and read a block's first rep with that in mind.
- The console's first-write hold drifts by the minute. Compare adjacent blocks, never arms pooled over a whole benchmark.
- A `/mnt/c` checkout under WSL puts Windows filesystem reads of the CIA inside the timed window. Measure from a clone in the Linux home.

## Compared with FBI and ftpd

The bench kit above tells whether a change made 3Drop faster or slower. This section describes a different measurement, the one behind the [speed tables in the README](../README.md#speed-compared-with-fbi-and-ftpd): 3Drop against FBI's Remote Install for installs, and against ftpd for files on the SD card. The scripts that ran it aren't in this repository, so here's what they did.

**Setup.** The console was an Old 3DS (system 11.17.0-50U, USA, Luma3DS v13.3.3) on its charger, near the Mac, on our home Wi-Fi's 2.4 GHz band (20 MHz; it speaks 802.11b/g only). The Mac, a MacBook Air (Apple M3) on the same network's 5 GHz band, ran every command: 3Drop's tools, `curl` for ftpd, and a small HTTP server for FBI to download from. FBI was 2.6.1 (title version 2145) and ftpd 3.2.1 (3105). We didn't measure how far the console was from the access point, or what else was using the network.

**Four runs.** 3Drop ran a release build with its default settings (signed requests, compression off). The first run, on 20 November 2026, used the build from just before v1's last two fixes: one for START while 3Drop waits on a request, one for keeping Wi-Fi up with the lid closed. Neither changes how 3Drop moves data. The second run, on 21 November, used v1's own code. It measured what the first didn't finish or got wrong: the 18 MB new install and both 106 MB installs (the first run stopped partway through the installs), and reading a file's size and listing a folder (the first run's checks wrongly failed every ftpd attempt at those two). It also added downloads and uploads of 32 MiB and 100 MiB. The second run stopped partway through the installs too, so a third run later that day, with the same build, measured the last two: the 18 MB update and the published-link install. Those three runs left 3Drop a little slower than ftpd at two tasks: reading a file's size (by about 0.01 s) and making a folder (by about 0.06 s). In case those gaps were noise, a fourth run on 23 November measured only those two again, with v1's release build, and their rows come from it. That day, both tools took more than twice as long to make a folder as in the first run. No task where 3Drop came out ahead was measured again. Every row not named above comes from the first run.

**The data.** The files were random bytes of 1 KiB, 1 MiB, 4 MiB, 32 MiB and 100 MiB, a folder of 200 files (6,890,122 bytes in ten folders) and a folder of 1,024 files of 16 bytes each. For installs there were test CIAs in three sizes (1,252,352, 17,898,496 and 105,852,416 bytes), each an app with random RomFS data, with an older and a newer version of one disposable title per size. The published-link row used the 71,181,248-byte CIA that the DDLC-LOVE project publishes as v1.1.9. Each attempt wrote to a new path: on the SD card for an upload, on the Mac for a download.

**What the clock covers.** Each attempt is timed on the Mac with a monotonic clock, from just before its command starts to its exit. Every command runs inside the same small `bash` wrapper, so start-up is in every time.

- **3Drop:** the whole `tools/device.sh` command (`get`, `put`, `ls`, `rename`, `delete` or `mkdir`, with `r` for a folder), or the whole `tools/drop.sh --new --no-bump` (`--update --no-bump` for an update). An install includes `drop.sh`'s look-up of the title's seed on Nintendo's server and, if it fires, its ghost retry (see [Outcomes](#outcomes)). The published-link row also times the Mac's download of the CIA and the check of its bytes.
- **ftpd:** already running and answering when the clock starts. One `curl` process over FTP (anonymous, passive) does the task, with `RETR`, `STOR`, `LIST`, `SIZE`, `DELE`, `MKD`, `RMD`, or `RNFR` and `RNTO`. A folder is one `curl` process on one control connection, handed the list of the 200 names; 3Drop's folder command finds the files itself.
- **FBI:** Remote Install is open when the clock starts: the script launched FBI, pressed down eleven times, then A twice. The clock includes sending FBI the URL, a fixed 1.5 s pause, the A that confirms the install (sent through Luma3DS's input redirection), FBI's download from the Mac (from the internet for the published link) and its install, and it stops when FBI sends its reply byte. If FBI's listener isn't open yet, the script retries the connection every 0.2 s for up to 5 s and leaves that wait out of FBI's time. The pause and the press add about 1.7 s to every FBI time. FBI deletes the old title before it installs (see [Updating 3Drop and the tools](configuration.md#updating-3drop-and-the-tools)), so its update rows delete and install, where 3Drop installs over the old version.
- **Left out, for every tool:** launching ftpd or FBI, FBI's navigation, the return to 3Drop, removing the previous title, installing the older version for an update row, and the checks after the attempt.
- **Interrupted transfers** are timed whole: the first attempt, cut off at 45 s, a 2 s pause, and the resumed transfer, so each of those times includes the 45 s and the 2 s. An attempt that finishes before the cut fails.

**Order and count.** Each task ran as five pairs, or three for the longest tasks (the folder upload, the 32 MiB and 100 MiB downloads and uploads, both interrupted transfers, both 106 MB installs and the published-link install): 3Drop once, then the other tool once. 3Drop went first in every pair. ftpd and FBI were launched fresh for each attempt, and after each one the script quit the app (START, with a few B presses first for FBI) and pressed A on the HOME Menu to bring 3Drop back, so 3Drop had just been relaunched when its next attempt began. No script restarted the console. Before every attempt the script checked that 3Drop answered as ready and that the console was on its charger. Each run began with two unscored trial attempts, an ftpd upload and an FBI install, which aren't in the tables.

**Failures.** An attempt fails if its command exits non-zero or runs past its limit (1,800 s, or for FBI 120 s plus 10 s per MB of the CIA), if the check afterwards finds wrong bytes, a wrong installed title or version, an incomplete listing, a second control connection in an ftpd folder run or a result that is not as expected, or if the script cannot get back to 3Drop. The one exception is an FBI listener that still refuses the connection after 5 s of retries: the script never reached FBI, so that counts as neither an attempt nor a failure, and its task is marked incomplete. FBI's reply byte means its request ended, not that the install worked, so an install counts only if the new version is installed afterwards. A failed attempt is charged the time from the start of its setup to the end of its recovery. The script never retries or replaces an attempt, and a task that did not get all its attempts is marked incomplete.

**The tables.** A tool's time for a task is the median of all its attempts, with a failed attempt ranked slower than any success. That is the third fastest of five and the second of three, and the median shows as failed when more than half the attempts failed. A cell also says how many attempts ran, when not all did, or how many failed. The last column gives the other tool's median divided by 3Drop's, from the unrounded medians, to one decimal (`3.0x faster`), or the share of time saved when that is under 1.1. A task is marked faster only if every planned attempt of both tools ran, 3Drop failed no more often than the other tool, and 3Drop's median and its total time per success (all its attempts' time, failed ones included, divided by its successes) are both lower. Otherwise the column says why: `incomplete`, `not run`, `tied`, `3Drop slower`, `3Drop failed more` or `failures cost more`. A send-from-memory diagnostic that needs a bench build isn't in the tables.

**To repeat it,** use an Old 3DS with Luma3DS (its input redirection started from Rosalina's menu, so the PC can press buttons), FBI and ftpd installed, 3Drop in front, and a PC with `curl`, `perl` and a small HTTP server. For each task and each pair: put the console in its starting state (the title absent or at the older version, fresh paths), time 3Drop's command, check the result, launch the other app, time its command, check, quit the app and bring 3Drop back. Keep every row, failures included, and take medians.
