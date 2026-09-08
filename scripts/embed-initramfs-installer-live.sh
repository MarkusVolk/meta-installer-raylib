#!/bin/sh
# Embeds the initramfs-based installer directly into an ALREADY
# RUNNING, already-deployed system's own ESP - no ".wic" file, no
# losetup, no mtools, unlike add-initramfs-installer-boot-entry.sh
# (build artifact, no real system yet). Run this ON the target
# machine itself, as root, with the kernel + initramfs.cpio.gz
# already transferred there.
#
# NOT usable via this project's own "RootFS Update" app feature -
# that targets the rootfs partition, not the ESP/boot partition this
# needs to write to instead.
set -eu

usage() {
    echo "Usage: $0 [-k kernel] [-i initramfs.cpio.gz] [esp-mountpoint]" >&2
    echo "  -k/-i: auto-detected from tmp/deploy/images/*/ (relative to cwd) if not" >&2
    echo "  given - same convention as the other two embedding scripts, though on the" >&2
    echo "  primary use case (running on an already-deployed target, not a build" >&2
    echo "  machine) there normally isn't one there and explicit paths are needed." >&2
    echo "  esp-mountpoint defaults to /boot." >&2
    exit 1
}

KERNEL_ARG=""
INITRAMFS_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -k) KERNEL_ARG="$2"; shift 2 ;;
        -i) INITRAMFS_ARG="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) break ;;
    esac
done

resolve_deploy_input() {
    # Relative to cwd - run this from your build directory. With more
    # than one tmp/deploy/images/<machine>/ directory present (a real,
    # observed case: MACHINE switched from intel-corei7-64 to
    # genericx86-64 in local.conf, the old machine's stale artifacts
    # still lying around), a plain glob loop silently returned the
    # alphabetically LAST match - i.e. the stale intel-corei7-64
    # initramfs, not the freshly built genericx86-64 one - and the
    # just-made image change never actually reached the target. So:
    # honor an exported MACHINE (same convention as build-payload-
    # image.sh) if set, otherwise take the NEWEST match by mtime,
    # which is what "the one I just built" means in practice.
    pattern="$1"
    found=""
    if [ -n "${MACHINE:-}" ]; then
        for f in tmp/deploy/images/"$MACHINE"/$pattern; do
            [ -e "$f" ] || continue
            found="$f"
        done
        echo "$found"
        return
    fi
    for f in tmp/deploy/images/*/$pattern; do
        [ -e "$f" ] || continue
        if [ -z "$found" ] || [ "$f" -nt "$found" ]; then
            found="$f"
        fi
    done
    echo "$found"
}

if [ -z "$KERNEL_ARG" ]; then
    KERNEL_ARG=$(resolve_deploy_input "*bzImage")
    [ -n "$KERNEL_ARG" ] || { echo "E: No kernel given and none auto-detected (looked for tmp/deploy/images/*/*bzImage)." >&2; usage; }
    echo "Auto-detected kernel: $KERNEL_ARG"
fi
if [ -z "$INITRAMFS_ARG" ]; then
    INITRAMFS_ARG=$(resolve_deploy_input "*installer-raylib-initramfs-*.rootfs.cpio.gz")
    [ -n "$INITRAMFS_ARG" ] || { echo "E: No initramfs given and none auto-detected." >&2; usage; }
    echo "Auto-detected initramfs: $INITRAMFS_ARG"
fi
ESP_MOUNT="${1:-/boot}"

[ "$(id -u)" = "0" ] || { echo "E: must run as root (writes into ${ESP_MOUNT})." >&2; exit 1; }
[ -f "$KERNEL_ARG" ] || { echo "E: Kernel not found: $KERNEL_ARG" >&2; exit 1; }
[ -f "$INITRAMFS_ARG" ] || { echo "E: Initramfs not found: $INITRAMFS_ARG" >&2; exit 1; }

# Sanity check: is $ESP_MOUNT actually a mounted vfat filesystem, not
# just some directory named /boot? Being wrong here would write the
# kernel/initrd into the wrong place entirely.
findmnt -n "$ESP_MOUNT" >/dev/null 2>&1 || { echo "E: ${ESP_MOUNT} is not a mountpoint at all." >&2; exit 1; }
ESP_FSTYPE=$(findmnt -n -o FSTYPE "$ESP_MOUNT")
if [ "$ESP_FSTYPE" != "vfat" ]; then
    echo "E: ${ESP_MOUNT} is mounted but its filesystem is '${ESP_FSTYPE}', not vfat - doesn't look like an ESP. Refusing to guess." >&2
    exit 1
fi
[ -w "$ESP_MOUNT" ] || { echo "E: ${ESP_MOUNT} is not writable (read-only mount?)." >&2; exit 1; }

echo "Target ESP: ${ESP_MOUNT} (vfat, confirmed mounted)"

mkdir -p "${ESP_MOUNT}/loader/entries"

# Distinct names so they can't collide with whatever kernel this
# already-running system itself boots from.
cp "$KERNEL_ARG" "${ESP_MOUNT}/installer-initramfs-bzImage"
cp "$INITRAMFS_ARG" "${ESP_MOUNT}/installer-initramfs.cpio.gz"

# No "root=" - the whole point of the initramfs variant.
cat > "${ESP_MOUNT}/loader/entries/initramfs-installer.conf" << EOF
title Installer (RAM-resident)
version installer-initramfs
linux /installer-initramfs-bzImage
initrd /installer-initramfs.cpio.gz
options console=ttyS0,115200 console=tty0
sort-key 9-installer
EOF

echo "Wrote initramfs installer boot entry (sort-key 9-installer)."

# Unlike add-initramfs-installer-boot-entry.sh (which renames the
# desktop's own entry to "Desktop" for consistent ordering), this
# system's own existing entries are deliberately left untouched -
# rewriting someone else's already-working boot configuration carries
# more risk than it's worth. Check the resulting boot menu order and
# adjust sort-key manually if needed.
echo "Done. Note: this system's own existing boot entry was left untouched (unlike this project's other two embedding scripts) - check the resulting boot menu order yourself."
