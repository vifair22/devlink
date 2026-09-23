#!/usr/bin/env bash
#
# Photograph the windowed agent without a Windows machine.
#
# Nothing in DEVLINKW.EXE can be reached from the test suite, which runs on
# Linux, so the window is checked by looking at it. Wine draws it on a headless
# X server, Xvfb writes its framebuffer out, and tools/xwd2png.py turns that
# into a PNG. What this proves is that the window comes up, the controls are
# created and the layout holds: it is Wine's idea of a common control, not
# Windows 2000's, so it is a smoke test and not a rendering reference.
#
# Neither Wine nor Xvfb is a build dependency, and this is in no gate.
#
# usage: tools/gui-preview.sh [OUT.png] [SECONDS]
set -u

out=${1:-build/preview/agent.png}
settle=${2:-4}
exe=build/bin/DEVLINKW.EXE

for tool in Xvfb wine python3; do
    command -v "$tool" >/dev/null || { echo "gui-preview: no $tool; skipped" >&2; exit 0; }
done
[ -x "$exe" ] || { echo "gui-preview: build $exe first (make agent)" >&2; exit 1; }

work=$(mktemp -d)
mkdir -p "$(dirname "$out")" "$work/fb"

# A display nobody else is on. Fixing on one number meant this quietly
# photographed whatever was already there when anything else held it.
display=
for n in $(seq 99 120); do
    [ -e "/tmp/.X11-unix/X$n" ] && continue
    display=":$n"
    break
done
[ -n "$display" ] || { echo "gui-preview: no free X display" >&2; exit 1; }

export WINEPREFIX="$work/wine" WINEDEBUG=-all DISPLAY=$display

cleanup() {
    [ -n "${wine_pid:-}" ] && wineserver -k 2>/dev/null
    [ -n "${xvfb_pid:-}" ] && kill "$xvfb_pid" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

Xvfb $display -screen 0 820x600x24 -fbdir "$work/fb" >"$work/xvfb.log" 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do
    [ -e "$work/fb/Xvfb_screen0" ] && break
    sleep 0.2
done
[ -e "$work/fb/Xvfb_screen0" ] || { echo "gui-preview: Xvfb did not start" >&2; exit 1; }

# A prefix of its own, so this neither reads nor writes the user's ~/.wine.
WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -u >"$work/wineboot.log" 2>&1

# COM1 will not open: Wine points it at /dev/ttyS0, which is either absent or
# not ours. That is the intended picture. It puts the failure in the log pane,
# which is itself worth seeing, and it keeps this from touching a real port.
setsid wine "$exe" COM1 115200 >"$work/wine.log" 2>&1 &
wine_pid=$!
sleep "$settle"

python3 tools/xwd2png.py "$work/fb/Xvfb_screen0" "$out"
