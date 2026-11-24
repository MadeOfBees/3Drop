#!/usr/bin/env bash
# shellcheck source=/dev/null disable=SC2154 # fakeDevice.sh and wire.sh are linted alone, not again in each test
# Seeds: the pinned fetch from the server, and the import.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fakeDevice.sh"
seed=000d0a0d0a00ff112233445566778899 # CR, LF and NUL bytes
fakeSeed 0004000000175e00 200 "$seed"
: >"$FAKE/seed-log"
expect "$(seedFetch 0004000000175E00)" "$seed" "a seed from the server"
case "$(cat "$FAKE/seed-log")" in
"1 -q -sS --max-time $SEED_TIMEOUT --cacert $ROOT/tools/nintendo-class2-ca-g3.pem --capath /dev/null --ssl-no-revoke -o "*" -w %{http_code} https://kagiya-ctr.cdn.nintendo.net/title/0x0004000000175e00/ext_key?country=US") ;;
*) fail "the seed request: $(cat "$FAKE/seed-log")" ;;
esac
out=$(seedFetch 0004000000033500) || fail "a title with no seed is not a failure"
expect "$out" "" "a title with no seed"
for answer in "200 ${seed}00" "200 ${seed%??}" "200 " "500 $seed"; do
	fakeSeed 0004000000175e01 "${answer% *}" "${answer#* }"
	silent seedFetch 0004000000175e01
done
fakeSeed 0004000000175e01 -
run 4 seedFetch 0004000000175e01
printf '0004000000175e00 0\n0004000000033500 0\n' >>"$FAKE/installed"
: >"$FAKE/log"
case "$("$ROOT/tools/device.sh" seed 127.0.0.1 0004000000175e00)" in "response "*" seed committed ok 00000000 0004000000175e00 added") ;; *) fail "device.sh seed" ;; esac
grep -qx "seed [0-9a-f]* [0-9a-f]* title:0004000000175e00 $seed" "$FAKE/log" || fail "the seed sent is not the one fetched"
case "$("$ROOT/tools/device.sh" seed 127.0.0.1 0004000000175e00)" in *" seed committed ok 00000000 0004000000175e00 present") ;; *) fail "a seed held already" ;; esac
hexDecode "$seed" >"$TMP/seed.dat"
case "$("$ROOT/tools/device.sh" seed 127.0.0.1 0004000000175e00 "$TMP/seed.dat")" in *" seed committed ok 00000000 0004000000175e00 present") ;; *) fail "a seed file" ;; esac
head -c 15 "$TMP/seed.dat" >"$TMP/short.dat"
fails "$ROOT/tools/device.sh" seed 127.0.0.1 0004000000175e00 "$TMP/short.dat"
out=$("$ROOT/tools/device.sh" seed 127.0.0.1 0004000e00175e00 "$TMP/seed.dat" 2>&1) && fail "a patch took a seed"
[[ "$out" == *" seed rejected not-application 00000000"$'\n'"device: a patch or DLC: this takes an application only" ]] || fail "a patch's seed: $out"
expect "$("$ROOT/tools/device.sh" seed 127.0.0.1 0004000000033500)" "0004000000033500 has no seed on Nintendo's server: nothing to import" "device.sh seed of a title with none"
expect "$(count seed)" 4 "seed requests (none for a short file or a title with no seed)"
# Here curl itself tests the pin against a throwaway CA: a seed is accepted only for that CA and the
# name it signed.
(
	unset -f curl
	tls=$TMP/tls
	mkdir -p "$tls/www"
	pids=() # the servers below, which stop with this group
	trap 'kill ${pids[@]+"${pids[@]}"} 2>/dev/null || :' EXIT
	cnf() { printf '[req]\ndistinguished_name = dn\nprompt = no\n[dn]\nCN = %s\n[ca]\nbasicConstraints = critical, CA:true\nkeyUsage = critical, keyCertSign\n[leaf]\nsubjectAltName = DNS:localhost\nextendedKeyUsage = serverAuth\n' "$1"; }
	cnf "3Drop test CA" >"$tls/ca.cnf"
	cnf localhost >"$tls/leaf.cnf"
	printf '0004000000176000 0\n' >>"$FAKE/installed"
	# A step back of the wall clock (WSL's: 1-3 s about every 30 s) between a certificate's minting and
	# the fetch leaves it "not yet valid" (curl 60), to the CA that vouches for it too. A run the clock
	# stepped across goes again (steadyRun, lib.sh) with new certificates, and the vouched fetch's checks
	# come after the loop. The refusals inside the loop are the same with a step or none.
	until steadyRun; do
		{ openssl req -x509 -config "$tls/ca.cnf" -extensions ca -newkey rsa:2048 -nodes -keyout "$tls/ca.key" -out "$tls/ca.pem" -days 2 &&
			openssl req -new -config "$tls/leaf.cnf" -newkey rsa:2048 -nodes -keyout "$tls/key.pem" -out "$tls/req.pem" &&
			openssl x509 -req -in "$tls/req.pem" -CA "$tls/ca.pem" -CAkey "$tls/ca.key" -set_serial 2 -days 2 \
				-extfile "$tls/leaf.cnf" -extensions leaf -out "$tls/cert.pem"; } >"$tls/made" 2>&1 || fail "the test certificates: $(cat "$tls/made")"
		printf 'HTTP/1.1 200 OK\r\nContent-Length: 16\r\n\r\n0123456789abcdef' >"$tls/www/0004000000176000"
		# LibreSSL's s_server doesn't take port 0 and exits quietly on a taken port, so try free ports
		# until one listens.
		for try in 1 2 3; do
			port=$(perl -MIO::Socket::INET -e 'print IO::Socket::INET->new(Listen => 1, LocalAddr => "127.0.0.1", LocalPort => 0)->sockport')
			(cd "$tls/www" && exec openssl s_server -HTTP -accept "$port" -cert ../cert.pem -key ../key.pem) </dev/null >"$tls/server" 2>&1 &
			server=$!
			pids+=("$server")
			for ((i = 0; i < 100; i++)); do
				if grep -q ACCEPT "$tls/server" || ! kill -0 "$server" 2>/dev/null; then break; fi
				sleep 0.1
			done
			! grep -q ACCEPT "$tls/server" || break
			[ "$try" != 3 ] || fail "no TLS server here: $(cat "$tls/server")"
		done
		: >"$FAKE/log"
		vouched=0
		SEED_CA=$tls/ca.pem SEED_URL="https://localhost:$port/TITLE" wireSeed 127.0.0.1 "$fakePort" 0004000000176000 >/dev/null 2>"$tls/vouched" || vouched=$?
		sent=$(grep -cx "seed [0-9a-f]* [0-9a-f]* title:0004000000176000 $(wireHex 0123456789abcdef)" "$FAKE/log") || :
		for wrong in "$SEED_CA localhost" "$tls/ca.pem 127.0.0.1"; do
			: >"$FAKE/log"
			st=0
			SEED_CA=${wrong% *} SEED_URL="https://${wrong#* }:$port/TITLE" wireSeed 127.0.0.1 "$fakePort" 0004000000176000 >/dev/null 2>"$tls/err" || st=$?
			expect "$st:$(count seed)" 1:0 "a seed from a server '$wrong' does not vouch for"
			grep -q "nothing went to the console: if this title is seed-encrypted, it will not boot until it has its seed (device.sh seed <ip> 0004000000176000 <seed-file> takes it from a 16-byte file)$" "$tls/err" ||
				fail "a server '$wrong' does not vouch for: $(cat "$tls/err")"
		done
	done
	[ "$vouched" = 0 ] || fail "a seed from a server its CA vouches for by name: $(cat "$tls/vouched")"
	[ "$sent" = 1 ] || fail "the seed sent is not the one the server gave"
	perl -MIO::Socket::INET -e '$s = IO::Socket::INET->new(Listen => 1, LocalAddr => "127.0.0.1", LocalPort => 0) or die;
		open(F, ">", $ARGV[0]) or die; print F $s->sockport; close F; $c = $s->accept; sleep 30' "$tls/mute" &
	pids+=("$!")
	waitFor "$tls/mute"
	# This checks the pin alone: a server the system's CAs vouch for (example.com, where this host
	# reaches it) is refused all the same, never taken for one with no seed.
	if curl -q -sS --max-time 10 -o /dev/null https://example.com 2>/dev/null; then
		st=0
		SEED_URL=https://example.com/TITLE seedFetch 0004000000176000 >/dev/null 2>&1 || st=$?
		[ "$st" != 0 ] || fail "a server only the system's CAs vouch for was trusted"
	else
		skip "example.com unreachable, so the pin against the system's CAs is unchecked"
	fi
	t0=$SECONDS st=0
	SEED_TIMEOUT=2 SEED_URL="https://127.0.0.1:$(cat "$tls/mute")/TITLE" seedFetch 0004000000176000 >/dev/null 2>&1 || st=$?
	expect "$st" 4 "a seed server that never answers"
	[ $((SECONDS - t0)) -le 10 ] || fail "a seed server that never answers held the fetch $((SECONDS - t0)) s"
	st=0
	out=$(hash -p "$tls/none" curl && seedFetch 0004000000176000 2>&1) || st=$?
	[[ "$st:$out" == "1:$TOOL: the seed was not fetched (no curl here to fetch it with), so nothing went to the console: if this title is seed-encrypted, "* ]] || fail "no curl: $st:$out"
	# A server that cuts the handshake (curl's 35) gives no answer, like one that never answers: both give 4.
	perl -MIO::Socket::INET -e '$s = IO::Socket::INET->new(Listen => 5, LocalAddr => "127.0.0.1", LocalPort => 0) or die;
		open(F, ">", $ARGV[0]) or die; print F $s->sockport; close F; while ($c = $s->accept) { sysread($c, $b, 1); close $c }' "$tls/cut" &
	pids+=("$!")
	waitFor "$tls/cut"
	st=0
	SEED_URL="https://127.0.0.1:$(cat "$tls/cut")/TITLE" seedFetch 0004000000176000 >/dev/null 2>"$tls/err" || st=$?
	[[ "$st:$(cat "$tls/err")" == "4:$TOOL: the seed was not fetched (curl: (35) "* ]] || fail "a seed server that cuts the handshake: $st:$(cat "$tls/err")"
)
echo "testSeeds: PASS"
