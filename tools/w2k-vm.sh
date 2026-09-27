#!/bin/bash
# Boot the A21m's own disk in QEMU as a Windows 2000 test machine for devlink,
# for when the ThinkPad itself is out of reach.
#
#   tools/w2k-vm.sh                     boot it headless, in the background
#   tools/w2k-vm.sh stop                power Windows off cleanly (through the agent)
#   tools/w2k-vm.sh shot [FILE]         screendump to a PNG (default shot.png)
#   tools/w2k-vm.sh reset-overlay [-y]  throw away every change made in the VM
#   MEM=256 FWD_PORT=12099 tools/w2k-vm.sh
#
# The disk is a qcow2 overlay on the raw image of the A21m's disk. That image
# is the only backup of the machine, so it is only ever a read-only backing
# file: QEMU opens a backing file read-only, and nothing here passes the image
# to QEMU directly. Every write lands in the overlay, which is disposable.
#
# Reaching it, once Windows is up (about two minutes from start):
#   DEVLINK_LINE=$VMDIR/com1 build/bin/devlink info     the agent on COM1
#   127.0.0.1:$FWD_PORT on the host                     port 2098 in the guest
# The guest is 10.0.2.15 by DHCP and sees the host as 10.0.2.2.
#
# Hardware that matters, found the hard way on the first boot:
# - The disk needs the A21m's BIOS geometry, 240 heads and 63 sectors, the
#   one its NTFS boot sector records. SeaBIOS guesses otherwise, the boot
#   code reads the wrong sectors and spins on a blank screen.
# - The south bridge is the PIIX4, as on the A21m. On QEMU's default PIIX3
#   the IDE controller has a PCI ID Windows was never set up for, and it
#   stops with INACCESSIBLE_BOOT_DEVICE.
# - Cirrus VGA and the AMD PCnet NIC both have drivers in the box.
set -euo pipefail

root=$(dirname "$(readlink -f "$0")")/..
vmdir=${VMDIR:-$HOME/vm/w2k-devlink}
image=${IMAGE:-$HOME/a21m-image/DJSA-210.img}
overlay=$vmdir/overlay.qcow2
mon=$vmdir/monitor.sock
pidfile=$vmdir/qemu.pid
mem=${MEM:-512}
fwd_port=${FWD_PORT:-12098}

die() { echo "w2k-vm: $*" >&2; exit 1; }

running() {
    [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null
}

# One human monitor command; the reply, without the banner and prompt echo.
monitor() {
    printf '%s\n' "$*" | socat -t2 - "UNIX-CONNECT:$mon" | tr -d '\r' |
        grep -v -e '^QEMU ' -e '^(qemu)' || true
}

# The overlay must sit on the image and nothing else, and must not be the
# image itself: a wrong IMAGE or a hand-made overlay stops here, not in QEMU.
check_overlay() {
    local backing
    [ "$(readlink -f "$overlay")" != "$(readlink -f "$image")" ] ||
        die "the overlay path is the raw image; refusing"
    backing=$(qemu-img info --output=json -U "$overlay" |
        sed -n 's/^ *"backing-filename": "\(.*\)",*$/\1/p' | head -n 1)
    [ "$(readlink -f "$backing")" = "$(readlink -f "$image")" ] ||
        die "$overlay is backed by '$backing', not $image"
}

cmd_start() {
    running && die "already running (pid $(cat "$pidfile")); '$0 stop' first"
    command -v socat >/dev/null || die "socat is needed for the monitor"
    mkdir -p "$vmdir"
    if [ ! -f "$overlay" ]; then
        [ -f "$image" ] || die "no image at $image"
        qemu-img create -q -f qcow2 -b "$image" -F raw "$overlay"
        echo "created $overlay on $image"
    fi
    check_overlay
    # A QEMU that died uncleanly leaves these, and the pty symlink refuses
    # to be created over an existing file.
    rm -f "$vmdir/com1" "$mon"

    # shellcheck disable=SC2054  # the commas are QEMU option syntax
    local args=(
        -name w2k-devlink -enable-kvm
        -M pc,x-south-bridge=piix4-isa -cpu pentium3 -m "$mem"
        -vga cirrus -display none
        -drive "file=$overlay,format=qcow2,if=none,id=hd0"
        -device ide-hd,drive=hd0,bus=ide.0,unit=0,lcyls=1024,lheads=240,lsecs=63
        -nic "user,model=pcnet,hostfwd=tcp:127.0.0.1:$fwd_port-:2098"
        -chardev "pty,id=com1,path=$vmdir/com1" -serial chardev:com1
        -monitor "unix:$mon,server,nowait"
        -daemonize -pidfile "$pidfile"
    )
    # Belt and braces for the one file that must never be written.
    local a
    for a in "${args[@]}"; do
        case $a in *"$(basename "$image")"*) die "the image is on the command line: $a" ;; esac
    done
    qemu-system-i386 "${args[@]}" >/dev/null

    cat <<EOF
started (pid $(cat "$pidfile")), $mem MB; the agent answers in about two minutes
  COM1:     $vmdir/com1
            DEVLINK_LINE=$vmdir/com1 build/bin/devlink info
  TCP:      127.0.0.1:$fwd_port on the host -> port 2098 in the guest (10.0.2.15)
  screen:   $0 shot [FILE]
  monitor:  socat - UNIX-CONNECT:$mon
  stop:     $0 stop
EOF
}

# Ask Windows for a clean power-off through the agent: WMI's Win32Shutdown
# with 8, power off, not forced. The power button is no substitute here:
# QEMU's system_powerdown turns this image off in a fifth of a second, with
# no "event log stopped" (6006) logged, so Windows is not shutting down.
poweroff_via_agent() {
    local devlink=$root/build/bin/devlink vbs
    [ -x "$devlink" ] || return 1
    DEVLINK_LINE=$vmdir/com1 timeout 15 "$devlink" info >/dev/null 2>&1 || return 1
    vbs=$(mktemp)
    printf '%s\r\n' \
        'Set w = GetObject("winmgmts:{impersonationLevel=impersonate,(Shutdown)}!\\.\root\cimv2")' \
        'For Each os In w.ExecQuery("select * from Win32_OperatingSystem")' \
        '  os.Win32Shutdown 8' \
        'Next' >"$vbs"
    DEVLINK_LINE=$vmdir/com1 timeout 15 "$devlink" put "$vbs" C:/WINNT/TEMP/W2KVMOFF.VBS --force >/dev/null 2>&1 ||
        { rm -f "$vbs"; return 1; }
    rm -f "$vbs"
    # The link drops as Windows goes down, so the exec's own status means
    # nothing; whether QEMU exits is the test.
    DEVLINK_LINE=$vmdir/com1 timeout 20 "$devlink" exec 'cscript //nologo C:\WINNT\TEMP\W2KVMOFF.VBS' >/dev/null 2>&1 || true
}

cmd_stop() {
    running || { echo "not running"; return 0; }
    local pid i how=agent
    pid=$(cat "$pidfile")
    if ! poweroff_via_agent; then
        how="power button"
        echo "the agent does not answer; pressing the power button, which is abrupt" >&2
        monitor system_powerdown >/dev/null
    fi
    for i in $(seq 1 "${STOP_TIMEOUT:-120}"); do
        kill -0 "$pid" 2>/dev/null || { echo "stopped ($how) after ${i} s"; return 0; }
        sleep 1
    done
    # Still up: most likely a dialog is holding the shutdown. Pulling the
    # plug risks only the overlay.
    monitor quit >/dev/null
    echo "Windows did not power off in ${STOP_TIMEOUT:-120} s; QEMU quit hard" >&2
    return 1
}

cmd_shot() {
    running || die "not running"
    local file
    file=$(readlink -f "${1:-shot.png}")
    monitor screendump "$file" -f png
    echo "$file"
}

cmd_reset_overlay() {
    running && die "running; '$0 stop' first"
    [ -f "$overlay" ] || { echo "no overlay"; return 0; }
    if [ "${1:-}" != "-y" ]; then
        read -r -p "delete $overlay (every change made in the VM)? [y/N] " ans
        [ "$ans" = y ] || [ "$ans" = Y ] || { echo "kept"; return 0; }
    fi
    rm -f -- "$overlay"
    echo "deleted $overlay; the next start makes a fresh one"
}

case ${1:-start} in
    start)         cmd_start ;;
    stop)          cmd_stop ;;
    shot)          cmd_shot "${2:-}" ;;
    reset-overlay) cmd_reset_overlay "${2:-}" ;;
    *) die "usage: $0 [start | stop | shot [FILE] | reset-overlay [-y]]" ;;
esac
