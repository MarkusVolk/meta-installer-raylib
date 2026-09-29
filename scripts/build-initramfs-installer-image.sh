#!/bin/sh
# Builds a bootable EFI image for the RAM-resident installer
# (core-image-installer-raylib-initramfs.bb): an ESP with the kernel,
# the small squashfs-boot initramfs and a systemd-boot entry, and an
# ext4 partition labeled images that holds the installer squashfs. The
# entry boots with squashfs.toram, so the installer runs from RAM.
set -eu

usage() {
    echo "Usage: $0 [-k kernel] [-i squashfs-boot-initramfs.cpio.gz] [-s installer.squashfs] [output.img]" >&2
    echo "  -k, -i and -s auto-detected from tmp/deploy/images/*/ (relative to cwd) if not given." >&2
    exit 1
}

KERNEL_ARG=""
INITRAMFS_ARG=""
SQUASHFS_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -k) KERNEL_ARG="$2"; shift 2 ;;
        -i) INITRAMFS_ARG="$2"; shift 2 ;;
        -s) SQUASHFS_ARG="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) break ;;
    esac
done
OUTPUT_IMG="${1:-installer-initramfs.img}"

for tool in sfdisk partx mkfs.vfat mkfs.ext4 losetup truncate mcopy mmd setsid; do
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
    INITRAMFS_ARG=$(resolve_deploy_input "squashfs-boot-initramfs-*.cpio.gz")
    [ -n "$INITRAMFS_ARG" ] || { echo "E: No initramfs given and none auto-detected." >&2; exit 1; }
    echo "Auto-detected initramfs: $INITRAMFS_ARG"
fi
if [ -z "$SQUASHFS_ARG" ]; then
    SQUASHFS_ARG=$(resolve_deploy_input "*installer-raylib-initramfs-*.rootfs.squashfs-zst")
    [ -n "$SQUASHFS_ARG" ] || { echo "E: No squashfs given and none auto-detected." >&2; exit 1; }
    echo "Auto-detected squashfs: $SQUASHFS_ARG"
fi
[ -f "$SQUASHFS_ARG" ] || { echo "E: Squashfs not found: $SQUASHFS_ARG" >&2; exit 1; }
[ -f "$KERNEL_ARG" ] || { echo "E: Kernel not found: $KERNEL_ARG" >&2; exit 1; }
[ -f "$INITRAMFS_ARG" ] || { echo "E: Initramfs not found: $INITRAMFS_ARG" >&2; exit 1; }

IMAGES_MB=$(( $(stat -c %s "$SQUASHFS_ARG") / 1048576 + 32 ))
echo "Building ${OUTPUT_IMG} (ESP plus an images partition, no rootfs partition)..."
rm -f "$OUTPUT_IMG"
truncate -s $((64 + IMAGES_MB + 2))M "$OUTPUT_IMG"
sfdisk --no-reread "$OUTPUT_IMG" << EOF
label: gpt
size=64M, type=U, bootable
size=${IMAGES_MB}M, type=L
EOF
IMAGES_PARTUUID=$(sfdisk --part-uuid "$OUTPUT_IMG" 2 | tr 'A-F' 'a-f')

STAGE=$(mktemp -d)
LOOP=""
trap 'rm -rf "$STAGE"; [ -z "$LOOP" ] || losetup -d "$LOOP" 2>/dev/null || true' EXIT
mkdir "${STAGE}/installer"
cp --reflink=auto "$SQUASHFS_ARG" "${STAGE}/installer/installer.squashfs"

LOOP=$(losetup -f --show -P "$OUTPUT_IMG")
partx -u "$LOOP" >/dev/null 2>&1 || true

ESP_PART="${LOOP}p1"
mkfs.vfat -F 32 -n BOOT "$ESP_PART"
mkfs.ext4 -q -L images -d "$STAGE" "${LOOP}p2"

setsid mmd -i "$ESP_PART" ::/loader < /dev/null
setsid mmd -i "$ESP_PART" ::/loader/entries < /dev/null
setsid mmd -i "$ESP_PART" ::/installer < /dev/null
setsid mcopy -i "$ESP_PART" "$KERNEL_ARG" ::/installer/kernel < /dev/null
setsid mcopy -i "$ESP_PART" "$INITRAMFS_ARG" ::/installer/initramfs < /dev/null

cat > "${STAGE}/loader.conf" << EOF
default installer
timeout 0
EOF
setsid mcopy -i "$ESP_PART" "${STAGE}/loader.conf" ::/loader/loader.conf < /dev/null

cat > "${STAGE}/installer.conf" << EOF
title Installer (initramfs, RAM-resident)
version installer-initramfs
linux /installer/kernel
initrd /installer/initramfs
options squashfs=PARTUUID=${IMAGES_PARTUUID}:/installer/installer.squashfs squashfs.toram console=ttyS0,115200 console=tty0
EOF
setsid mcopy -i "$ESP_PART" "${STAGE}/installer.conf" ::/loader/entries/installer.conf < /dev/null

losetup -d "$LOOP"
LOOP=""
echo "Done: ${OUTPUT_IMG}"
echo "Write it to a USB stick with: sudo dd if=${OUTPUT_IMG} of=/dev/sdX bs=4M status=progress conv=fsync"
