#!/bin/sh
# Builds a bootable EFI image for the initramfs-based installer
# installer variant (core-image-installer-raylib-initramfs.bb) - an
# ESP-only image (kernel + initramfs, no rootfs partition), with a
# hand-written systemd-boot loader entry instead of wic's own
# bootimg-efi plugin (that plugin derives root=PARTUUID= from a
# "part /" reference, which doesn't exist here - everything runs from
# the initramfs in RAM).
set -eu

usage() {
    echo "Usage: $0 [-k kernel] [-i initramfs.cpio.gz] [output.img]" >&2
    echo "  Both -k and -i auto-detected from tmp/deploy/images/*/ (relative to cwd) if not given." >&2
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
OUTPUT_IMG="${1:-installer-initramfs.img}"

for tool in sfdisk partx mkfs.vfat losetup truncate mcopy mmd setsid; do
    command -v "$tool" >/dev/null 2>&1 || { echo "E: required tool not found: $tool" >&2; exit 1; }
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
    [ -n "$KERNEL_ARG" ] || { echo "E: No kernel given and none auto-detected (looked for tmp/deploy/images/*/*bzImage)." >&2; exit 1; }
    echo "Auto-detected kernel: $KERNEL_ARG"
fi
if [ -z "$INITRAMFS_ARG" ]; then
    INITRAMFS_ARG=$(resolve_deploy_input "*installer-raylib-initramfs-*.rootfs.cpio.gz")
    [ -n "$INITRAMFS_ARG" ] || { echo "E: No initramfs given and none auto-detected." >&2; exit 1; }
    echo "Auto-detected initramfs: $INITRAMFS_ARG"
fi
[ -f "$KERNEL_ARG" ] || { echo "E: Kernel not found: $KERNEL_ARG" >&2; exit 1; }
[ -f "$INITRAMFS_ARG" ] || { echo "E: Initramfs not found: $INITRAMFS_ARG" >&2; exit 1; }

echo "Building ${OUTPUT_IMG} (ESP-only, no rootfs partition)..."
rm -f "$OUTPUT_IMG"
truncate -s 256M "$OUTPUT_IMG"
sfdisk --no-reread "$OUTPUT_IMG" << EOF
label: gpt
size=+, type=U, bootable
EOF

LOOP=$(losetup -f --show -P "$OUTPUT_IMG")
trap 'losetup -d "$LOOP" 2>/dev/null || true' EXIT
partx -u "$LOOP" >/dev/null 2>&1 || true

ESP_PART="${LOOP}p1"
mkfs.vfat -F 32 -n BOOT "$ESP_PART"

setsid mmd -i "$ESP_PART" ::/loader < /dev/null
setsid mmd -i "$ESP_PART" ::/loader/entries < /dev/null
setsid mcopy -i "$ESP_PART" "$KERNEL_ARG" ::/bzImage < /dev/null
setsid mcopy -i "$ESP_PART" "$INITRAMFS_ARG" ::/initramfs.cpio.gz < /dev/null

cat > /tmp/.initramfs-loader.conf.$$ << EOF
default installer
timeout 0
EOF
setsid mcopy -i "$ESP_PART" /tmp/.initramfs-loader.conf.$$ ::/loader/loader.conf < /dev/null
rm -f /tmp/.initramfs-loader.conf.$$

# No "options root=..." - the whole point of this variant. console=
# kept for both serial and local display, same as the disk-backed
# variant's own entries.
cat > /tmp/.initramfs-entry.conf.$$ << EOF
title Installer (initramfs, RAM-resident)
version installer-initramfs
linux /bzImage
initrd /initramfs.cpio.gz
options console=ttyS0,115200 console=tty0
EOF
setsid mcopy -i "$ESP_PART" /tmp/.initramfs-entry.conf.$$ ::/loader/entries/installer.conf < /dev/null
rm -f /tmp/.initramfs-entry.conf.$$

losetup -d "$LOOP"
trap - EXIT
echo "Done: ${OUTPUT_IMG}"
echo "Write it to a USB stick with: sudo dd if=${OUTPUT_IMG} of=/dev/sdX bs=4M status=progress conv=fsync"
