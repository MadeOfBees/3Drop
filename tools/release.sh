#!/usr/bin/env bash
# release.sh number <tag>: `<N> <title version>` for a release tag v1..v63; any other tag fails, so
#   nothing is built or published. Release N is title version N x 1024 (deploy.sh numbers the
#   builds between), and N is its store revision.
# release.sh check <tag> <published> <tags>: whether that release may publish now, given the
#   published releases' tags and every tag, a file of lines each: never a release published already,
#   and never one that skips a number no tag holds, since a mistaken high tag would spend every band
#   below it for good. A number skipped for a tag whose release failed may be passed.
# release.sh latest <published>: the published release GitHub marks Latest, the one the update QR and
#   the store fetch: the highest number, whatever order the releases were published in.
# release.sh assets: the files a release publishes, a name a line, SHA256SUMS apart (it lists them).
# release.sh sums <dir>: <dir>/SHA256SUMS, a `sha256sum` line for each of those files, all of which
#   must be in <dir>: a release is never published without one.
# release.sh notes <owner/repo>: the paragraph every release page opens with, how to install and
#   update, and where the source is; GitHub's generated notes follow it (`gh release create --notes` prepends).
set -euo pipefail
. "$(dirname "$0")/lib.sh"

# releaseNumber <tag>: prints N, or returns 1 for a tag that belongs to no release.
releaseNumber() {
	if ! [[ "$1" =~ ^v([1-9][0-9]?)$ ]] || [ "${BASH_REMATCH[1]}" -gt 63 ]; then return 1; fi
	echo "${BASH_REMATCH[1]}"
}

# highest <file>: prints the highest release number among its tags, or 0 for none.
highest() {
	local tag n top=0
	while read -r tag; do
		n=$(releaseNumber "$tag") || continue
		[ "$n" -le "$top" ] || top=$n
	done <"$1"
	echo "$top"
}

# assets: prints the files a release publishes, one name per line (not counting SHA256SUMS). One
# list serves the sums, the create step and the paragraph's own pin in testTools. The CIA and its
# ELF are one build (check.sh).
assets() { printf '%s\n' 3drop.cia 3drop.elf 3drop-qr.png 3drop.unistore 3drop-library-sources.tar.gz 3drop-library-sources.txt; }

# sha256 <file>: prints its SHA-256 the way the tools take one, with openssl (which every tool
# needs; a Mac has no sha256sum).
sha256() {
	local out
	out=$(openssl dgst -sha256 <"$1") || return
	echo "${out##* }"
}

# notes <owner/repo>: prints the same text for every release of a repository.
notes() {
	local text
	IFS= read -r -d '' text <<'EOF' || :
## Install and update

- **Install:** on the 3DS, open FBI's Remote Install and scan the code in the README's [Install 3Drop on the 3DS](https://github.com/@REPO@#1-install-3drop-on-the-3ds), or copy `3drop.cia` to the SD card and install it with FBI. For Universal Updater, add the store `https://github.com/@REPO@/releases/latest/download/3drop.unistore`.
- **Update:** in 3Drop, press Y and scan that same code (3Drop takes only a newer release), or install the newer `3drop.cia` with FBI or Universal Updater, or send it with `tools/drop.sh --self 3drop.cia <ip>`.
- **The console app and the tools come from the same release,** so each understands the other (if their protocols differ, only `stats` and a self-update still work between them). After you install or update 3Drop, put your clone of the tools on that release: `git clone --branch <tag> https://github.com/@REPO@.git` the first time, `git fetch --tags && git checkout <tag>` after, `<tag>` being the release 3Drop's top screen names.
- **The other files:** `3drop.unistore` is the store manifest Universal Updater reads. `3drop-qr.png` is this release's own code for `3drop.cia`. `3drop.elf` is the debug build the CIA was made from. `SHA256SUMS` lists the SHA-256 of every other file here (`sha256sum -c SHA256SUMS`, or `shasum -a 256 -c SHA256SUMS` on macOS). `3drop-library-sources.tar.gz` holds the exact sources of the libraries linked into 3Drop and devkitPro's build recipes for them, and `3drop-library-sources.txt` lists their versions, licenses, SHA-256s and upstream links.
- **Source and license:** 3Drop is free software under GPL-3.0-or-later. The **Source code** archives on this page are its source at this release's tag ([the repository](https://github.com/@REPO@)), and `3drop-library-sources.tar.gz` holds the libraries'.
EOF
	printf '%s\n' "${text//@REPO@/$1}"
}

case "${1:-}:$#" in
number:2)
	n=$(releaseNumber "$2") || die "release tags are v1 to v63, with no leading 0 or suffix: $2 is not one, so nothing is built"
	echo "$n $((n * 1024))"
	;;
check:4)
	n=$(releaseNumber "$2") || die "$2 is not a release tag (v1 to v63)"
	! grep -qxF -e "$2" "$3" || die "$2 is published already, and a release never changes: a fix goes out as the next number"
	top=$(highest "$3")
	for ((k = top + 1; k < n; k++)); do
		grep -qxF -e "v$k" "$4" || die "$2 skips v$k, which no tag holds: the next release is v$((top + 1)), and a higher one would spend the title versions of every release below it"
	done
	;;
latest:2)
	top=$(highest "$2")
	[ "$top" != 0 ] || die "no release is published"
	echo "v$top"
	;;
assets:1) assets ;;
sums:2)
	cd "$2"
	for file in $(assets); do
		[ -f "$file" ] || die "$file is not in $2, and a release is never published without it"
	done
	# SHA256SUMS is written whole, after every file is known to be there; sha256sum -c reads these
	# lines. The sum is taken separately from the echo, so if it fails the step stops instead of
	# writing a bare name.
	for file in $(assets); do
		sum=$(sha256 "$file")
		echo "$sum  $file"
	done >SHA256SUMS
	;;
notes:2) notes "$2" ;;
*) usage ;;
esac
