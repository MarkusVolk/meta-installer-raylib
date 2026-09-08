#!/bin/bash
# Collects everything needed to diagnose the "GUI reacts to neither
# keyboard nor mouse" problem on real hardware - and, since the switch
# to the genericx86-64 machine, the "iwd.service does not start"
# report as well - and writes it to a USB stick, so the bundle can be
# handed over from the target machine.
#
# Intended flow (yocto-rootfs-updater-raylib.service is installed but
# not enabled while this is being debugged - SYSTEMD_AUTO_ENABLE =
# "disable" in its recipe, "systemctl start" brings it up by hand):
# boot, log in as root on tty1, plug in a USB stick, run
# "/root/collect-logs.sh". Every stage
# that needs your interaction announces itself and counts down.
#
# Target directory: the first argument if given, otherwise /mnt/usb
# (the stable symlink usb-automount keeps for the first mounted USB
# partition). usb-automount mounts read-only on purpose (rescue
# context - never write to payload media), so the stick is remounted
# read-write just for this script and switched back to read-only at
# the end. If nothing is auto-mounted, the first partition on a
# USB-attached disk (lsblk TRAN=usb) is mounted read-write at
# /mnt/usb-debug instead.
#
# Tool choices are dictated by what the image actually contains
# (coreutils, util-linux pieces, systemd - no busybox, no procps, no
# findutils, no pciutils/usbutils, no evtest): kernel log via
# "journalctl -k", process list via /proc/*/comm, PCI/USB inventory
# straight from /sys, raw evdev capture via dd|od.

set -u

TARGET="${1:-}"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
GUI_BIN="/usr/bin/yocto-rootfs-updater-raylib"
EVDEV_CAPTURE_SECONDS=10
GUI_TEST_SECONDS=30
REMOUNT_RO_AT_END=0
UMOUNT_AT_END=0

say() { printf '\n==> %s\n' "$*"; }
warn() { printf 'W: %s\n' "$*" >&2; }
die() { printf 'E: %s\n' "$*" >&2; exit 1; }

# Runs a command, writing its combined output to $OUT/<name>.txt with
# the command line as first line. Never aborts the script - a missing
# tool just leaves a short error inside that one file.
capture() {
    local name="$1"; shift
    {
        printf '$ %s\n\n' "$*"
        "$@" 2>&1
    } > "$OUT/$name.txt"
}

[ "$(id -u)" = "0" ] || die "must run as root"

# ---------------------------------------------------------------- target
if [ -z "$TARGET" ]; then
    if mountpoint -q /mnt/usb 2>/dev/null; then
        TARGET=/mnt/usb
    else
        say "nothing auto-mounted at /mnt/usb - looking for a USB partition"
        usb_part=""
        # No TRAN column in this listing on purpose: it is empty for
        # partitions and "read" would collapse the blank field,
        # shifting TYPE into its place. TRAN is asked of the parent
        # disk separately below.
        while read -r name type; do
            [ "$type" = "part" ] || continue
            disk="$(lsblk -no PKNAME "/dev/$name" 2>/dev/null)"
            [ -n "$disk" ] || continue
            disk_tran="$(lsblk -dno TRAN "/dev/$disk" 2>/dev/null)"
            if [ "$disk_tran" = "usb" ]; then
                usb_part="/dev/$name"
                break
            fi
        done < <(lsblk -rno NAME,TYPE)
        [ -n "$usb_part" ] || die "no USB partition found - plug in a stick and rerun, or pass a directory as argument"
        TARGET=/mnt/usb-debug
        mkdir -p "$TARGET"
        mount -o rw "$usb_part" "$TARGET" || die "mounting $usb_part at $TARGET failed"
        UMOUNT_AT_END=1
        say "mounted $usb_part at $TARGET"
    fi
fi

[ -d "$TARGET" ] || die "$TARGET is not a directory"

if mountpoint -q "$TARGET" 2>/dev/null; then
    opts="$(findmnt -no OPTIONS "$TARGET" 2>/dev/null)"
    case ",$opts," in
        *,ro,*)
            mount -o remount,rw "$TARGET" || die "remounting $TARGET read-write failed"
            REMOUNT_RO_AT_END=1
            ;;
    esac
fi

OUT="$TARGET/installer-logs-$STAMP"
mkdir -p "$OUT" || die "cannot create $OUT"
say "writing to $OUT"

# ----------------------------------------------------------- static state
say "collecting system state"
capture uname uname -a
capture cmdline cat /proc/cmdline
capture version cat /proc/version
capture os-release cat /etc/os-release
capture systemd-version systemctl --version
capture uptime cat /proc/uptime
capture modules cat /proc/modules
capture mounts findmnt
capture lsblk lsblk -o NAME,TRAN,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT
capture config-toml cat /etc/yocto-rootfs-updater-raylib/config.toml
capture gui-service-file cat /usr/lib/systemd/system/yocto-rootfs-updater-raylib.service
capture etc-systemd-system ls -laR /etc/systemd/system
capture systemctl-failed systemctl --no-pager --failed
capture systemctl-units systemctl --no-pager list-units --all
capture systemctl-unit-files systemctl --no-pager list-unit-files
capture systemctl-status-gui systemctl --no-pager status yocto-rootfs-updater-raylib.service
capture systemctl-status-getty systemctl --no-pager status 'getty@tty1.service' 'getty@tty2.service'
capture active-vt cat /sys/class/tty/tty0/active
capture fb-devices sh -c 'for f in /sys/class/graphics/fb*; do echo "$f: $(cat "$f/name" 2>/dev/null)"; done'
capture dev-dri ls -la /dev/dri /dev/dri/by-path
capture dev-input ls -la /dev/input /dev/input/by-id /dev/input/by-path
capture proc-bus-input-devices cat /proc/bus/input/devices
capture udev-db udevadm info -e
capture journal journalctl -b --no-pager -o short-monotonic
capture journal-kernel journalctl -b -k --no-pager -o short-monotonic
capture journal-udev journalctl -b --no-pager -u systemd-udevd
capture journal-gui journalctl -b --no-pager -u yocto-rootfs-updater-raylib.service

# WiFi / iwd. iwd's own startup check (check_crypto() in its main.c)
# probes the kernel's AF_ALG crypto sockets and refuses to start when
# hash/skcipher support is missing - the exact option names it wants
# end up in its journal, so that unit's log is the first thing to read.
# No rfkill/iw/ip tools in this image: interface, PHY and rfkill state
# come straight from sysfs, the driver/firmware chatter from the kernel
# log. The kernel/crypto listing shows whether af_alg & co. are even
# present as modules (they are not, once "kernel-modules" is blocked).
capture systemctl-status-iwd systemctl --no-pager status iwd.service
capture journal-iwd journalctl -b --no-pager -o short-monotonic -u iwd.service
capture journal-wifi-kernel sh -c 'journalctl -b -k --no-pager -o short-monotonic | grep -i -E "wlan|wifi|80211|rtw89|iwlwifi|firmware|rfkill"'
capture net-devices ls -la /sys/class/net /sys/class/ieee80211
capture crypto-modules sh -c 'ls -la /lib/modules/$(uname -r)/kernel/crypto /lib/modules/$(uname -r)/kernel/net/wireless /lib/modules/$(uname -r)/kernel/drivers/net/wireless 2>&1'
capture proc-crypto cat /proc/crypto
{
    for rf in /sys/class/rfkill/rfkill*; do
        [ -e "$rf" ] || continue
        echo "$(basename "$rf"): name=$(cat "$rf/name" 2>/dev/null) type=$(cat "$rf/type" 2>/dev/null) state=$(cat "$rf/state" 2>/dev/null) soft=$(cat "$rf/soft" 2>/dev/null) hard=$(cat "$rf/hard" 2>/dev/null)"
    done
} > "$OUT/rfkill.txt"
{
    for n in /sys/class/net/*; do
        [ -e "$n" ] || continue
        echo "$(basename "$n"): operstate=$(cat "$n/operstate" 2>/dev/null) carrier=$(cat "$n/carrier" 2>/dev/null) address=$(cat "$n/address" 2>/dev/null) driver=$(basename "$(readlink "$n/device/driver" 2>/dev/null)" 2>/dev/null) wireless=$([ -d "$n/wireless" ] && echo yes || echo no)"
    done
} > "$OUT/net-interfaces.txt"

# Per-device evdev details straight from sysfs plus udevadm's view.
{
    for ev in /sys/class/input/event*; do
        [ -e "$ev" ] || continue
        dev="$ev/device"
        echo "=================== $(basename "$ev")"
        echo "name:  $(cat "$dev/name" 2>/dev/null)"
        echo "phys:  $(cat "$dev/phys" 2>/dev/null)"
        echo "uniq:  $(cat "$dev/uniq" 2>/dev/null)"
        echo "id:    bus=$(cat "$dev/id/bustype" 2>/dev/null) vendor=$(cat "$dev/id/vendor" 2>/dev/null) product=$(cat "$dev/id/product" 2>/dev/null) version=$(cat "$dev/id/version" 2>/dev/null)"
        for cap in "$dev"/capabilities/*; do
            echo "cap/$(basename "$cap"): $(cat "$cap" 2>/dev/null)"
        done
        echo "--- udevadm info"
        udevadm info -q all -n "/dev/input/$(basename "$ev")" 2>&1
        echo
    done
} > "$OUT/input-devices.txt"

# Who has /dev/input/* or /dev/dri/* open right now - if something
# else grabbed the event devices, raylib would never see events.
{
    for fd in /proc/[0-9]*/fd/*; do
        link="$(readlink "$fd" 2>/dev/null)" || continue
        case "$link" in
            /dev/input/*|/dev/dri/*|/dev/tty*)
                pid="${fd#/proc/}"; pid="${pid%%/*}"
                echo "$link  <-  pid $pid ($(cat "/proc/$pid/comm" 2>/dev/null))"
                ;;
        esac
    done | sort
} > "$OUT/open-device-fds.txt"

{
    for p in /proc/[0-9]*; do
        pid="${p#/proc/}"
        printf '%6s  %s\n' "$pid" "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || cat "$p/comm" 2>/dev/null)"
    done
} > "$OUT/processes.txt"

{
    for d in /sys/bus/pci/devices/*; do
        [ -e "$d" ] || continue
        echo "$(basename "$d") class=$(cat "$d/class") vendor=$(cat "$d/vendor") device=$(cat "$d/device") driver=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)"
    done
} > "$OUT/pci-devices.txt"

{
    for d in /sys/bus/usb/devices/*; do
        [ -e "$d/idVendor" ] || continue
        echo "$(basename "$d") $(cat "$d/idVendor"):$(cat "$d/idProduct") '$(cat "$d/manufacturer" 2>/dev/null)' '$(cat "$d/product" 2>/dev/null)' speed=$(cat "$d/speed" 2>/dev/null) driver=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)"
        for intf in "$d"/"$(basename "$d")":*; do
            [ -e "$intf" ] || continue
            echo "    $(basename "$intf") class=$(cat "$intf/bInterfaceClass" 2>/dev/null) sub=$(cat "$intf/bInterfaceSubClass" 2>/dev/null) proto=$(cat "$intf/bInterfaceProtocol" 2>/dev/null) driver=$(basename "$(readlink "$intf/driver" 2>/dev/null)" 2>/dev/null)"
        done
    done
} > "$OUT/usb-devices.txt"

# ---------------------------------------------- interactive: raw evdev
# Reads every event device that reports EV_KEY (bit 1) or EV_REL (bit
# 2) in its capabilities/ev mask for a few seconds in parallel. If the
# kernel delivers events at all, the per-device files fill up with
# input_event structs (24 bytes each on 64-bit). dd writes unbuffered,
# od only flushes at EOF when dd's timeout closes the pipe - that's
# why it is dd|od and not a plain "timeout od".
say "evdev capture: for the next $EVDEV_CAPTURE_SECONDS seconds, press a few keys and move/click the mouse"
sleep 2
stty -echo 2>/dev/null || true
pids=""
for ev in /sys/class/input/event*; do
    [ -e "$ev" ] || continue
    evmask="$(cat "$ev/device/capabilities/ev" 2>/dev/null || echo 0)"
    evmask=$((16#$evmask))
    if [ $((evmask & 6)) -ne 0 ]; then
        name="$(basename "$ev")"
        (
            timeout "$EVDEV_CAPTURE_SECONDS" dd if="/dev/input/$name" bs=24 status=none 2>/dev/null \
                | od -An -tx1 -v -w24 > "$OUT/evdev-raw-$name.txt"
        ) &
        pids="$pids $!"
    fi
done
for i in $(seq "$EVDEV_CAPTURE_SECONDS" -1 1); do
    printf '\r   %2d s left ' "$i"
    sleep 1
done
printf '\r                 \r'
wait $pids 2>/dev/null || true
for f in "$OUT"/evdev-raw-event*.txt; do
    [ -e "$f" ] || continue
    n=$(wc -l < "$f")
    echo "   $(basename "$f" .txt): $n events"
done

# ----------------------------------------------- interactive: GUI test
# Starts the real app for a limited time with its trace output (raylib
# INPUT:/DISPLAY: lines, the app's own log) captured line-buffered, so
# nothing is lost when the timeout kills it. The screen switches to
# the DRM framebuffer for the duration - just try keyboard and mouse
# in the GUI meanwhile, it comes back on its own.
if [ -x "$GUI_BIN" ]; then
    say "GUI test: the app starts in 3 seconds and is killed after $GUI_TEST_SECONDS seconds - try keyboard and mouse in it"
    sleep 3
    {
        echo "$ timeout $GUI_TEST_SECONDS $GUI_BIN   (started $(date))"
        echo
        timeout -s TERM -k 5 "$GUI_TEST_SECONDS" stdbuf -oL -eL "$GUI_BIN"
        rc=$?
        echo
        echo "exit status: $rc  (124 = killed by timeout as intended)"
    } > "$OUT/gui-test-output.txt" 2>&1
    capture journal-after-gui-test journalctl -b --no-pager -o short-monotonic --since "-2min"
    capture open-device-fds-after-gui ls -la /dev/input
    echo "   GUI test done ($(wc -l < "$OUT/gui-test-output.txt") lines of output)"
else
    warn "$GUI_BIN not found - skipping GUI test"
fi

# Drain whatever the keyboard test typed into this terminal so it
# doesn't end up as shell commands after the script exits.
stty sane 2>/dev/null || true
while read -r -t 1 _; do :; done

# ------------------------------------------------------------- wrap up
say "packing"
capture ls-out ls -la "$OUT"
( cd "$TARGET" && tar -czf "installer-logs-$STAMP.tar.gz" "installer-logs-$STAMP" )
sync

if [ "$REMOUNT_RO_AT_END" = 1 ]; then
    mount -o remount,ro "$TARGET" 2>/dev/null || warn "could not remount $TARGET read-only again"
fi
if [ "$UMOUNT_AT_END" = 1 ]; then
    umount "$TARGET" 2>/dev/null || warn "could not unmount $TARGET"
fi

say "done: $OUT  and  $TARGET/installer-logs-$STAMP.tar.gz"
echo "   The stick can be removed now."
