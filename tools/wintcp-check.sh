#!/usr/bin/env bash
#
# Run the Win32 TCP transport under Wine, against itself and against the POSIX
# one, with no Windows machine. tools/wintcp_check.c says what each mode
# checks; both builds come from that one file, so the two ends run the same
# test code over the two socket layers.
#
# Wine is not a build dependency and this is in no gate; it skips itself where
# Wine is missing.
#
# usage: tools/wintcp-check.sh WINTCP.EXE NATIVE_BINARY
set -u

exe=$1
native=$2
command -v wine >/dev/null || { echo "wintcp-check: no wine; skipped" >&2; exit 0; }

work=$(mktemp -d)
# A prefix of its own, so this neither reads nor writes the user's ~/.wine.
export WINEPREFIX="$work/wine" WINEDEBUG=-all
cleanup() {
    wineserver -k 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT
WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -u >"$work/wineboot.log" 2>&1

fail=0
bytes=1500000

# Starts an echo server in the background and sets port to what it bound.
start_echo() {
    local log=$1
    shift
    "$@" echo 0 >"$log" 2>&1 &
    echo_pid=$!
    for _ in $(seq 1 100); do
        port=$(sed -n 's/^listening on \([0-9]*\).*/\1/p' "$log" | tr -d '\r')
        [ -n "$port" ] && return 0
        sleep 0.1
    done
    echo "wintcp-check: echo server did not start:" >&2
    cat "$log" >&2
    return 1
}

echo "== Win32 against itself"
wine "$exe" self | tr -d '\r'
[ "${PIPESTATUS[0]}" -eq 0 ] || fail=1

echo "== POSIX client -> Win32 echo server"
if start_echo "$work/wecho.log" wine "$exe"; then
    "$native" send 127.0.0.1 "$port" "$bytes" || fail=1
    wait "$echo_pid" || fail=1
    tr -d '\r' <"$work/wecho.log"
else
    fail=1
fi

echo "== Win32 client -> POSIX echo server"
if start_echo "$work/pecho.log" "$native"; then
    wine "$exe" send 127.0.0.1 "$port" "$bytes" | tr -d '\r'
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1
    wait "$echo_pid" || fail=1
    cat "$work/pecho.log"
else
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "wintcp-check: FAILED"
    exit 1
fi
echo "wintcp-check: all passed"
