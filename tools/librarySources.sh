#!/usr/bin/env bash
# librarySources.sh <out-dir>: <out-dir>/3drop-library-sources.tar.gz, the exact sources of the
#   libraries 3Drop links with devkitPro's build recipes for them, and 3drop-library-sources.txt, the
#   manifest of their versions, licenses, SHA-256s and links. tools/librarySources.tsv names them.
#   Nothing is written unless the build image holds the versions that file names and every download
#   matches its SHA-256, so a release never goes out with the wrong sources or none. Needs docker,
#   curl and tar. release.yml runs it before the gate; tools/librarySources.tsv says how to bump a
#   library.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
[ "$#" = 1 ] || usage
[ -d "$1" ] || die "$1 is not a folder"
out=$(CDPATH='' cd "$1" && pwd)
list=${THREEDROP_LIBRARY_SOURCES:-$ROOT/tools/librarySources.tsv}
name=3drop-library-sources
# The image every build runs in, as build.sh reads it.
image=$(sed -n 's/^FROM //p' "$ROOT/tools/lint.Dockerfile")
[ -n "$image" ] || die "no FROM in tools/lint.Dockerfile"
scratch

# sha256 <file>: prints its SHA-256 as the tools take one. It uses openssl, which every tool needs;
# a Mac has no sha256sum.
sha256() {
	local out
	out=$(openssl dgst -sha256 <"$1") || return
	echo "${out##* }"
}

# The image's versions are checked first, because a mismatch is the cheap way to stop, before any download.
docker run --rm "$image" dkp-pacman -Q >"$SCRATCH/packages" || die "could not read the packages of $image: docker run --rm $image dkp-pacman -Q failed"
libraries=0
while IFS=$'\t' read -r kind package version _; do
	[ "$kind" = library ] || continue
	libraries=$((libraries + 1))
	have=$(awk -v p="$package" '$1 == p { print $2 }' "$SCRATCH/packages")
	[ "$have" = "$version" ] ||
		die "the build image has $package ${have:-not at all}, but tools/librarySources.tsv says $version: put the image's version in that file, with its source's URL and SHA-256 and its recipe's commit"
done <"$list"
[ "$libraries" != 0 ] || die "tools/librarySources.tsv names no library"

# Download each file from the URL, then its fallback, and keep it only if it is the one the list pins.
: >"$SCRATCH/got"
while IFS=$'\t' read -r kind package type file sha url fallback; do
	[ "$kind" = file ] || continue
	[[ "$sha" =~ ^[0-9a-f]{64}$ ]] || die "tools/librarySources.tsv: the SHA-256 of $file is not 64 lowercase hex digits"
	dir=$SCRATCH/$name/${type}s/$package
	mkdir -p "$dir"
	used=""
	# shellcheck disable=SC2086 # URLs hold no spaces
	for from in $url $fallback; do
		if ! curl --fail --location --silent --show-error --retry 3 --max-time 300 --output "$dir/$file" "$from" 2>"$SCRATCH/curl.err"; then
			echo "$TOOL: $from: $(cat "$SCRATCH/curl.err")" >&2
			continue
		fi
		got=$(sha256 "$dir/$file")
		if [ "$got" = "$sha" ]; then
			used=$from
			break
		fi
		echo "$TOOL: $from gave SHA-256 $got, not the $sha that tools/librarySources.tsv pins" >&2
	done
	[ -n "$used" ] || die "no source for $file matches its SHA-256 $sha: if upstream changed it, find out why first; then put the new SHA-256, or a URL that works, on its line in tools/librarySources.tsv"
	other=$(printf '%s\n' "$url" ${fallback:+"$fallback"} | grep -vxF "$used" || :)
	printf '%s\011%s\011%s\011%s\011%s\011%s\011%s\n' "$package" "$type" "$file" "$sha" "$(fileSize "$dir/$file")" "$used" "$other" >>"$SCRATCH/got"
done <"$list"

# A library with no source, whatever else was kept, is a release without its sources.
while IFS=$'\t' read -r kind package _; do
	[ "$kind" = library ] || continue
	awk -F'\t' -v p="$package" '$1 == p && $2 == "source" { found = 1 } END { exit !found }' "$SCRATCH/got" ||
		die "tools/librarySources.tsv has no source file for $package"
done <"$list"

{
	echo "The libraries 3Drop links: exact sources and build recipes"
	echo
	echo "3Drop is built in this image, and the versions below are what dkp-pacman -Q gives in it:"
	echo "  $image"
	echo "Every SHA-256 below was checked against its download when this archive was made. \"or\" is another"
	echo "URL for the file: tried only if needed, and accepted only at the same SHA-256. The files are"
	echo "stored as downloaded, under sources/ and recipes/ in the archive. 3Drop's own source, and quirc's"
	echo "(src/quirc/, vendored unmodified), are the repository at this release's tag."
	while IFS=$'\t' read -r kind package version license home recipe; do
		[ "$kind" = library ] || continue
		echo
		echo "$package $version"
		echo "  license:           $license"
		echo "  upstream:          $home"
		echo "  devkitPro recipe:  $recipe"
		awk -F'\011' -v p="$package" '$1 == p {
			printf "  %ss/%s/%s\n    SHA-256 %s, %s bytes\n    from %s\n", $2, p, $3, $4, $5, $6
			if ($7 != "") printf "    or %s\n", $7
		}' "$SCRATCH/got"
	done <"$list"
} >"$SCRATCH/$name/MANIFEST.txt"

# The manifest also goes out by itself, so a reader can see the versions without the 8 MB.
tar -C "$SCRATCH" -cf - "$name" | gzip -n >"$out/$name.tar.gz"
cp "$SCRATCH/$name/MANIFEST.txt" "$out/$name.txt"
echo "$TOOL: $out/$name.tar.gz is $(fileSize "$out/$name.tar.gz") bytes, $(wc -l <"$SCRATCH/got" | tr -d ' ') files of $libraries libraries"
