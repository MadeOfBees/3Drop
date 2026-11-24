# Building

```sh
tools/build.sh                         # release build in the pinned devkitARM image (needs Docker)
THREEDROP_TEST_VERBS=1 tools/build.sh  # bench build: adds the `bench` verb
BUILD_NUMBER=<n> tools/makeCIA.sh      # packages 3drop.cia at title version n (needs bannertool and makerom)
tools/deploy.sh <ip>                   # build, package, self-update the console, wait for it
```

- `makeCIA.sh` writes `BUILD_NUMBER` (0–65535, default 0) into the TMD (title metadata) as the title version. A self-update must be newer than the installed version, or it is refused as `not-newer`.
- Release vN is title version N × 1024. `deploy.sh` numbers each build one past the installed one, so builds deployed on vN sit between vN and vN+1. The next release installs over them by QR or `--self`.
- After 1023 deploys on one release, `deploy.sh` stops. Install the next release, or reinstall a release with FBI.
- A plain `makeCIA.sh` gives version 0, which `--self` refuses over any installed 3Drop. FBI, which delete-installs, takes it.
- `bannertool` and `makerom` come from `$BANNERTOOL` and `$MAKEROM`, your `PATH`, or `tools/bin/`. They are not bundled.
- CI downloads checksum-pinned copies (`tools/bootstrapCIPackagingTools.sh`), each a Linux x86-64 build:
  - bannertool 1.2.3 from [3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool/releases)
  - makerom v0.19.0 from [Project_CTR](https://github.com/3DSGuy/Project_CTR/releases)
  - 3dstool v1.2.6 from [3dstool](https://github.com/dnasdw/3dstool/releases), which the tests and the bench kit use (`$THREEDSTOOL`, your `PATH`, or `tools/bin/`)
- The release build compiles into `build/` and the bench build into `build-bench/`. The root `3drop.elf` is whichever was built last.

## 3Drop's title ID

The front page's [Back to 3Drop when your app exits](../README.md#back-to-3drop-when-your-app-exits) sets the chainloader to this title.

- The title ID `0004000003D37200` is 3Drop's. It comes from the `UniqueId` `0x3D372` in `res/buildCIA.rsf`, and the Makefile hands it to the source as `SELF_TITLE_ID`.
- The Testing Tool, 3Drop's test title, does the same (`tools/bench/testingTool.c`, `aptSetChainloader(SELF_TITLE_ID, MEDIATYPE_SD)`). It does so only when it leaves on its own (START, or after its unattended script), not when HOME closes it. It turns sleep off as it starts, and leaves it off for the jump.

## Tests

`tools/check.sh` runs lint, both builds, a test CIA, packaging and the host tests. The host tests (`tools/tests/run.sh`) compile the device sources on the host against libctru stubs, and run the real host scripts against a fake device. The check script also runs the host scripts' tests again on Linux, in the lint image, on the tree read-only. That run is `run.sh --linux`, and it takes every shell test without a `# host-only` line. A test passes only once it prints `<name>: PASS`. A check a host cannot make is listed after the run as `SKIPPED: <test>: <why>`.

- It runs on Linux, macOS and WSL. Under Git Bash on Windows, some tests skip checks they can't make there, and that fails the gate. It needs Docker and the packaging tools above. The host tests also need a C compiler, mbedtls and zlib (`build-essential libmbedtls-dev zlib1g-dev` on Linux, `brew install mbedtls@3` on macOS).
- It needs `3dstool` too. Without it the fixture test skips a check, and the gate fails on any skip but the few it lists.
- `CHECK_FAST=1` builds incrementally and skips the test CIA and packaging.
- `CHECK_EXPORT=1 tools/check.sh` runs it on HEAD's tracked files alone. It exports them with `git archive` into a fresh repository, as a published copy starts out.
- Lint runs in `3drop-lint:bookworm`. That image is built from `tools/lint.Dockerfile` on the pinned devkitARM image, the one every build runs in.

## Publishing releases

- A `v*` tag runs `.github/workflows/release.yml`. It builds `3drop.cia`, `3drop.elf` (the debug build the CIA was made from), a QR code image (`3drop-qr.png`) and the stamped `3drop.unistore`. It adds `SHA256SUMS`, `3drop-library-sources.tar.gz` and `3drop-library-sources.txt`, then publishes them as a GitHub Release. The release page opens with a fixed paragraph on installing, updating, the license and the source, then the generated notes.
- **Tags:** release tags are v1, v2, … v63, one number up each time. The workflow refuses any other `v*` name. It also refuses a tag whose number skips a number that no tag holds. The tag's number is the title version's major (see [Building](#building)) and the store's revision, so adding a tag renumbers nothing.
- **A release never changes.** A tag already published builds nothing new, so a mistake ships as the next number. Latest is always the highest published release.
- Tags pushed together publish one at a time, each in turn, and none is canceled (up to 100 queued). Push at most three at once, because GitHub starts no run when a push carries more tags than that.
- **Space releases at least a minute apart.** Two releases stamped in the same minute tie in Universal Updater's update check. What Universal Updater reads from the manifest is under [Updating 3Drop and the tools](configuration.md#updating-3drop-and-the-tools).
- To check a download, run `sha256sum -c SHA256SUMS` in the folder that holds the files (`shasum -a 256 -c SHA256SUMS` on macOS). SHA-256 shows a damaged or wrong download. It is not a signature.
- On a library bump, the header of `tools/librarySources.tsv` lists the steps. The release stops, naming the library, if the build image and that file disagree.
