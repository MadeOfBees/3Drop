#!/usr/bin/env bash
# makeFixture.sh refuses a size or unique id it cannot build a fixture for (makerom would silently
# stamp another title for a malformed id; no application has a system title's or 3Drop's own), builds
# the Testing Tool again when something it is built from is newer, and gives the title its usage
# names by default; a rebuild never rewrites the fixture a reader holds, and a bare id builds the
# title it names.
# host-only: the real builds need docker, makerom, bannertool and 3dstool.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
# shellcheck source=tools/lib.sh
. "$ROOT/tools/lib.sh"
# shellcheck source=tools/cia.sh
. "$ROOT/tools/cia.sh"
tool="$ROOT/tools/bench/makeFixture.sh"
# skipRest <why>: the checks above passed; the rest cannot run here.
skipRest() { skip "$1" && echo "$TEST_NAME: PASS" && exit 0; }
# build <what> <makeFixture.sh args>: runs a build and, when it fails, reports what it said last.
build() {
	local what=$1 out
	shift
	out=$("$tool" "$@" 2>&1) || fail "$what failed: $(tail -n 3 <<<"$out")"
}

# Ids that must be refused: one that is not hex; a whole title id; 0x100000, one past 20 bits; 16
# digits, which bash reads as -1, and 17, which wrap into range; 0x2ff, a system title's; and 3Drop's
# own, however it is spelled.
expect "$(selfTitleID)" 0004000003d37200 "3Drop's own title, whose unique id is refused below"
for bad in zzz 000400000ff71100 100000 c00001 ffffffffffffffff 1000000000000ff780 2ff 0 0x3D372 0003d372; do
	run 2 "$tool" 1 "$bad"
done
run 2 "$tool" 1 3d372
said "3Drop's own" "3Drop's own unique id"
run 2 "$tool" 1234567890 # ten digits: the size times a unit could wrap
said "9 digits" "a size of ten digits"
run 2 "$tool"
said 000400000ff43000 "the usage, with the default id's title"

# The rest of makeFixture.sh runs with packaging tools that only write the files the next step reads,
# in a copy of what it reads whose build.sh only records its arguments.
(
	mirror=$TMP/mirror fake=$TMP/fake template=$TMP/template.cia
	mkdir -p "$mirror/tools" "$mirror/res" "$mirror/src" "$fake"
	cp "$ROOT"/tools/*.sh "$mirror/tools/"
	cp -R "$ROOT/tools/bench" "$mirror/tools/bench"
	cp "$ROOT"/res/*.rsf "$mirror/res/"
	: >"$mirror/src/x"
	: >"$mirror/Makefile"
	: >"$mirror/tools/lint.Dockerfile"
	# Times for touch -t: what the tool is built from, the tool, and an edit made after.
	old=202001010000 elf=202101010000 new=202201010000
	cat >"$mirror/tools/build.sh" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$TMP/build.args"
touch -t $elf "$mirror/testingTool.elf"
EOF
	# Each writes the files -o names; makerom's CIA is the template with the title of the rsf's UniqueId.
	cat >"$fake/bannertool" <<'EOF'
#!/usr/bin/env bash
while [ $# -gt 1 ]; do [ "$1" != -o ] || : >"$2"; shift; done
EOF
	cat >"$fake/makerom" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
while [ $# -gt 1 ]; do
	case $1 in -f) kind=$2 ;; -o) out=$2 ;; -rsf) rsf=$2 ;; esac
	shift
done
[ "$kind" = cia ] || exec cp /dev/null "$out"
. "$FAKE_ROOT/tools/lib.sh"
. "$FAKE_ROOT/tools/cia.sh"
uid=$(sed -n 's/^[[:blank:]]*UniqueId[[:blank:]]*:[[:blank:]]*0x\([0-9A-Fa-f]*\).*/\1/p' "$rsf")
cp "$FAKE_CIA" "$out"
ciaLayout "$out"
filePatch "$out" $((ciaTmdBody + CIA_TMD_TITLE_ID)) "$(uidTitleID "$uid")"
EOF
	# A flush of the whole host, a dozen times over, is not part of this.
	printf '#!/usr/bin/env bash\n' >"$fake/sync"
	chmod +x "$mirror/tools/build.sh" "$fake/bannertool" "$fake/makerom" "$fake/sync"
	# shellcheck source=/dev/null disable=SC2034 # fakeDevice.sh is linted alone, and reads FAKE: set, it starts no device
	out=$(FAKE=$TMP/none && . "$TESTS/fakeDevice.sh" && fakeCia "$template" 0004000000000000 1 2>&1) ||
		fail "making a CIA to copy: $(tail -n 3 <<<"$out")"
	export FAKE_ROOT=$ROOT FAKE_CIA=$template BANNERTOOL=$fake/bannertool MAKEROM=$fake/makerom
	# shellcheck disable=SC2030 # this half's own, which its subshell keeps from the real half below
	export PATH="$fake:$PATH" THREEDROP_FIXTURE_DIR=$TMP/fixtures-fake
	tool=$mirror/tools/bench/makeFixture.sh
	title() { ciaTitleID "$THREEDROP_FIXTURE_DIR/fixture0m.cia"; }

	touch -t "$old" "$mirror/src" "$mirror/src/x" "$mirror/Makefile" "$mirror/tools/lint.Dockerfile" \
		"$mirror/tools/bench/testingTool.c" "$mirror/res/buildCIA.rsf" "$mirror/tools/cia.sh"
	build "the first fixture" 0
	expect "$(cat "$TMP/build.args")" "-B TARGET=testingTool all" "the Testing Tool's first build"
	expect "$(title)" 000400000ff43000 "the default id's title"
	# The ends of the range are applications'.
	rm "$TMP/build.args"
	build "uid 300" 0 300
	expect "$(title)" 0004000000030000 "the title of uid 300"
	build "uid fffff" 0 fffff
	expect "$(title)" 000400000fffff00 "the title of uid fffff"
	[ ! -e "$TMP/build.args" ] || fail "a Testing Tool newer than what it is built from was built again"
	# Each thing it is built from builds it again when it is edited afterwards.
	for input in src/x tools/bench/testingTool.c Makefile tools/lint.Dockerfile res/buildCIA.rsf tools/cia.sh; do
		touch -t "$new" "$mirror/$input"
		build "a build after $input changed" 0
		[ -e "$TMP/build.args" ] || fail "a newer $input did not build the Testing Tool again"
		rm "$TMP/build.args"
		touch -t "$old" "$mirror/$input"
	done
)

# shellcheck disable=SC2031 # the real half's own: the fake half's stayed in its subshell
export THREEDROP_FIXTURE_DIR=$TMP/fixtures
resolveCIATools >/dev/null 2>&1 || skipRest "no makerom or bannertool, so no fixture is really built"
# A rebuild replaces the fixture by a rename, so a rep still reading the old one (another arm of its
# size) reads it whole.
build "building a fixture to rebuild" 0 ff781
cp "$THREEDROP_FIXTURE_DIR/fixture0m.cia" "$TMP/old.cia"
exec 3<"$THREEDROP_FIXTURE_DIR/fixture0m.cia"
build "rebuilding a fixture" 0 ff782
cmp -s - "$TMP/old.cia" <&3 || fail "a rebuild rewrote the fixture a reader held"
exec 3<&-
expect "$(ciaTitleID "$THREEDROP_FIXTURE_DIR/fixture0m.cia")" 000400000ff78200 "the rebuilt fixture's title"
[ -x "$ciaThreeDsTool" ] || skipRest "no 3dstool, so a bare uid's title is not read back"
build "building a fixture with a bare uid" 1 ff780
expect "$(ciaTitleID "$THREEDROP_FIXTURE_DIR/fixture1m.cia")" 000400000ff78000 "the title bare uid 'ff780' built"
echo "testMakeFixtureUid: PASS"
