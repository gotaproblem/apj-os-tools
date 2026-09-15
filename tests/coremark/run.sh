#!/bin/sh
# CoreMark, built through OUR porting layer, on the build host.
#
# The point is not the score - it is the four CRCs. CoreMark validates
# itself, and it only validates if the port got the data types exactly
# right. Getting ee_u32 wrong (long is 32 bits on m68k and 64 on this
# host) changes the list and state CRCs and CoreMark says "Errors
# detected" instead of a number. That is a mistake worth catching here
# rather than on the Atari.
#
# It also checks the five EEMBC source files still match the md5s
# published in coremark.md5, because the whole claim to a comparable
# score rests on them being untouched.
set -e
cd "$(dirname "$0")/../../psctrl/coremark"

echo "--- published md5s (coremark.h is stale UPSTREAM; the five .c are what matter)"
md5sum -c coremark.md5 2>&1 | grep -E "core_.*\.c" || true
for f in core_main.c core_list_join.c core_matrix.c core_state.c core_util.c; do
	md5sum -c coremark.md5 2>/dev/null | grep -q "^$f: OK" || {
		echo "FAIL $f does not match its published md5 - it has been edited"
		exit 1
	}
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
D="-I. -DCM_HOST_TEST=1 -DPERFORMANCE_RUN=1 -DFLAGS_STR=\"-O2\" -DITERATIONS=0"

echo "--- building"
# shellcheck disable=SC2086
${CC:-cc} -O2 -Wall $D -o "$TMP/cm" \
	core_main.c core_list_join.c core_matrix.c core_state.c core_util.c \
	core_portme.c cm_shared.c

echo "--- running (this takes ~15 s: CoreMark sizes itself past its own"
echo "    10 second minimum, and a run under that is not reportable)"
OUT=$("$TMP/cm")
echo "$OUT" | tail -8

fail=0
echo "$OUT" | grep -q "Correct operation validated" || {
	echo "FAIL CoreMark did not validate - the port has the data types wrong"
	fail=1
}
# The reference CRCs for seeds 0,0,0x66 at 2000 bytes. crcfinal is NOT
# among them: it is chained across iterations (results->crc is folded in
# once per iteration), so it depends on how many iterations the run did -
# and this run auto-sizes itself to the host. 0x65c5 is the value for the
# fixed-iteration validation run only. The three below are the ones that
# say the port got its data types right, and they are the three CoreMark
# checks itself before printing "Correct operation validated".
for want in "crclist       : 0xe714" "crcmatrix     : 0x1fd7" \
            "crcstate      : 0x8e3a"; do
	echo "$OUT" | grep -q "$want" || { echo "FAIL wrong $want"; fail=1; }
done
echo "$OUT" | grep -q "^CoreMark 1.0 :" || { echo "FAIL no compliance line"; fail=1; }

secs=$(echo "$OUT" | sed -n 's/^Total time (secs): *\([0-9]*\).*/\1/p')
[ -n "$secs" ] && [ "$secs" -ge 10 ] || {
	echo "FAIL ran for ${secs}s - under the 10 s minimum, so not reportable"
	fail=1
}

[ $fail -eq 0 ] && echo "all checks passed" || echo "CHECKS FAILED"
exit $fail
