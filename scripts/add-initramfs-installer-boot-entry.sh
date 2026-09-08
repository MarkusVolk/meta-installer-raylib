#!/bin/sh
# Embeds the initramfs-based installer (core-image-installer-raylib-
# initramfs.bb) into an EXISTING desktop image's own ESP - kernel +
# initramfs cpio.gz copied in as two extra files, plus a new
# systemd-boot loader entry (no "root=", nothing to mount).
#
# No partition table work at all - just two extra files and a loader
# entry on the existing ESP.
set -eu

usage() {
    echo "Usage: $0 [-k kernel] [-i initramfs.cpio.gz] [-o output.img]" >&2
    echo "  -k/-i/-o: auto-detected/derived if not given (see below). No" >&2
    echo "  positional arguments accepted." >&2
    echo "  -k/-i: auto-detected from tmp/deploy/images/*/ (relative to cwd) if not given." >&2
    echo "  desktop.wic: always auto-discovered from payload_source_dir (env or" >&2
    echo "  ~/.config/build-payload-image.conf) - no explicit override for it at" >&2
    echo "  all." >&2
    echo "  Without -o: derived from desktop.wic's filename." >&2
    exit 1
}

KERNEL_ARG=""
INITRAMFS_ARG=""
OUTPUT_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -k) KERNEL_ARG="$2"; shift 2 ;;
        -i) INITRAMFS_ARG="$2"; shift 2 ;;
        -o) OUTPUT_ARG="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "E: Unknown/unexpected argument: $1 (only -k/-i/-o flags are accepted, no positional arguments)" >&2; usage ;;
    esac
done

for tool in sfdisk partx mkfs.vfat losetup truncate mcopy mmd setsid blkid findmnt; do
    command -v "$tool" >/dev/null 2>&1 || { echo "E: required tool not found: $tool" >&2; exit 1; }
done

unmount_udisks_automounts() {
    for p in "$1"p*; do
        [ -b "$p" ] || continue
        MP=$(findmnt -n -o TARGET "$p" 2>/dev/null || true)
        if [ -n "$MP" ]; then
            echo "Note: $p was auto-mounted at $MP (likely udisks2/gvfs) - unmounting so this script can access it directly."
            umount "$p" 2>/dev/null || umount -l "$p" 2>/dev/null || true
        fi
    done
}

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

# --- desktop.wic / output.img resolution (payload_source_dir,
# SUDO_USER-aware config path). ---
REAL_HOME="$HOME"
if [ -n "${SUDO_USER:-}" ] && command -v getent >/dev/null 2>&1; then
    SUDO_USER_HOME=$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)
    [ -n "$SUDO_USER_HOME" ] && REAL_HOME="$SUDO_USER_HOME"
fi
CONFIG_FILE="${BUILD_PAYLOAD_IMAGE_CONFIG:-$REAL_HOME/.config/build-payload-image.conf}"
[ -f "$CONFIG_FILE" ] && . "$CONFIG_FILE"

[ -n "${payload_source_dir:-}" ] || { echo "E: payload_source_dir not set (desktop.wic has no explicit override, only auto-discovery)." >&2; exit 1; }
echo "Searching for desktop.wic in ${payload_source_dir}..."
DESKTOP_IMG=""
for f in "$payload_source_dir"/*.wic; do
    [ -e "$f" ] || continue
    DESKTOP_IMG="$f"
done
[ -n "$DESKTOP_IMG" ] || { echo "E: No *.wic found under ${payload_source_dir}." >&2; exit 1; }
echo "Automatically found: $DESKTOP_IMG"
[ -f "$DESKTOP_IMG" ] || { echo "E: Desktop image not found: $DESKTOP_IMG" >&2; exit 1; }

if [ -n "$OUTPUT_ARG" ]; then
    OUTPUT_IMG="$OUTPUT_ARG"
else
    DIR=$(dirname "$DESKTOP_IMG")
    BASE=$(basename "$DESKTOP_IMG")
    STEM="${BASE%.*}"
    EXT=".${BASE##*.}"
    OUT_DIR="${payload_output_dir:-$DIR}"
    OUTPUT_IMG="${OUT_DIR}/${STEM}-with-initramfs-installer${EXT}"
    echo "No output filename given, using automatically: ${OUTPUT_IMG}"
fi

echo "Copying ${DESKTOP_IMG} to ${OUTPUT_IMG}..."
mkdir -p "$(dirname "$OUTPUT_IMG")"
# world-writable + sticky bit (like /tmp): anyone can write here, but
# only each file's own owner (or root) can delete/rename it.
chmod 1777 "$(dirname "$OUTPUT_IMG")" 2>/dev/null || true
cp --sparse=always "$DESKTOP_IMG" "$OUTPUT_IMG"

DESKTOP_LOOP=$(losetup -f --show -P "$OUTPUT_IMG")
trap 'losetup -d "$DESKTOP_LOOP" 2>/dev/null || true' EXIT
partx -u "$DESKTOP_LOOP" >/dev/null 2>&1 || true
unmount_udisks_automounts "$DESKTOP_LOOP"

# --- Find the desktop's own ESP - just locating the existing FAT
# partition to write two extra files and a loader entry into. ---
DESKTOP_ESP_PART=""
for p in "${DESKTOP_LOOP}"p*; do
    [ -b "$p" ] || continue
    FSTYPE=$(blkid -o value -s TYPE "$p" 2>/dev/null || true)
    [ "$FSTYPE" = "vfat" ] && DESKTOP_ESP_PART="$p" && break
done
[ -n "$DESKTOP_ESP_PART" ] || { echo "E: Could not find the desktop image's ESP (FAT partition)." >&2; exit 1; }
echo "Desktop ESP: ${DESKTOP_ESP_PART}"

# Distinct names so they can't collide with the desktop's own kernel.
setsid mcopy -D o -i "$DESKTOP_ESP_PART" "$KERNEL_ARG" ::/installer-initramfs-bzImage < /dev/null
setsid mcopy -D o -i "$DESKTOP_ESP_PART" "$INITRAMFS_ARG" ::/installer-initramfs.cpio.gz < /dev/null

setsid mdir -i "$DESKTOP_ESP_PART" ::/loader/entries < /dev/null >/dev/null 2>&1 || setsid mmd -i "$DESKTOP_ESP_PART" ::/loader/entries < /dev/null 2>/dev/null || true

# No "root=" - the whole point of this variant.
cat > /tmp/.initramfs-entry.conf.$$ << EOF
title Installer (RAM-resident)
version installer-initramfs
linux /installer-initramfs-bzImage
initrd /installer-initramfs.cpio.gz
options console=ttyS0,115200 console=tty0
sort-key 9-installer
EOF
setsid mcopy -D o -i "$DESKTOP_ESP_PART" /tmp/.initramfs-entry.conf.$$ ::/loader/entries/initramfs-installer.conf < /dev/null
rm -f /tmp/.initramfs-entry.conf.$$
echo "Wrote initramfs installer boot entry (sort-key 9-installer, sorts after the desktop's own entry)."

# --- Rename the desktop's own default entry to "Desktop" and give it
# a sort-key too: systemd-boot sorts entries WITH a sort-key before
# those WITHOUT one, regardless of value - giving only the new entry
# one would make ordering WORSE, not better. ---
DESKTOP_DEFAULT_ENTRY=$(setsid mcopy -i "$DESKTOP_ESP_PART" ::/loader/loader.conf - < /dev/null 2>/dev/null | \
    grep '^default ' | awk '{print $2}' || true)
case "$DESKTOP_DEFAULT_ENTRY" in
    ""|@*)
        DESKTOP_DEFAULT_ENTRY=$(setsid mdir -i "$DESKTOP_ESP_PART" -b ::/loader/entries/ < /dev/null 2>/dev/null | \
            grep -v initramfs-installer.conf | head -n 1 | sed -n 's#.*/\([^/]*\)\.conf$#\1#p' || true)
        ;;
esac
if [ -n "$DESKTOP_DEFAULT_ENTRY" ]; then
    setsid mcopy -i "$DESKTOP_ESP_PART" "::/loader/entries/${DESKTOP_DEFAULT_ENTRY}.conf" /tmp/.desktop-entry.$$ < /dev/null 2>/dev/null || true
    if [ -f /tmp/.desktop-entry.$$ ]; then
        sed -i '/^title /d; /^sort-key /d' /tmp/.desktop-entry.$$
        { echo "title Desktop"; cat /tmp/.desktop-entry.$$; echo "sort-key 0-desktop"; } > /tmp/.desktop-entry-new.$$
        setsid mcopy -D o -i "$DESKTOP_ESP_PART" /tmp/.desktop-entry-new.$$ "::/loader/entries/${DESKTOP_DEFAULT_ENTRY}.conf" < /dev/null
        rm -f /tmp/.desktop-entry.$$ /tmp/.desktop-entry-new.$$
        echo "Desktop's own boot entry renamed to 'Desktop', sort-key added so it shows before the installer entry."
    fi
else
    echo "W: Could not determine the desktop's own default boot entry - leaving its title/sort-key untouched."
fi

losetup -d "$DESKTOP_LOOP"
trap - EXIT
DESKTOP_LOOP=""

if [ -n "${SUDO_USER:-}" ]; then
    SUDO_USER_GROUP=$(id -gn "$SUDO_USER" 2>/dev/null || echo "$SUDO_USER")
    chown "${SUDO_USER}:${SUDO_USER_GROUP}" "$OUTPUT_IMG" 2>/dev/null || true
fi

echo "Done: ${OUTPUT_IMG}"
