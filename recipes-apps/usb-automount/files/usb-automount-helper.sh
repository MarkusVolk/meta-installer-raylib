#!/bin/sh
# Called by usb-automount@.service on mount and on that same service's
# stop (umount, e.g. device removed - see its own BindsTo=).
#
# Mount point is "/mnt/usb-<kernel-device-name>" (e.g. /mnt/usb-sda1),
# not a numbered "/mnt/usb0" - each kernel device name is already
# unique, and main.cpp's discover_local_file_candidate_dirs() matches
# any /mnt/ subdirectory starting with "usb", which this satisfies too.
#
# Additionally keeps a stable "/mnt/usb" symlink pointing at the FIRST
# mounted partition - a predictable path to type/script against,
# since sticks usually carry just one partition anyway. First one
# wins while it stays mounted; on its umount the link moves to any
# other still-mounted usb-* partition, else it's removed. main.cpp
# skips the bare "usb" name on purpose so the link doesn't show up as
# a duplicate of the real mount point it points to.
#
# Mounted read-only throughout - a rescue/installer context reads
# payload files off external media, never writes back to it.
set -e

ACTION="$1"
DEV_NAME="$2"
DEV="/dev/${DEV_NAME}"
MP="/mnt/usb-${DEV_NAME}"
LINK="/mnt/usb"

# True if $LINK is a symlink whose target is currently mounted.
link_valid() {
    [ -L "$LINK" ] && mountpoint -q "$LINK" 2>/dev/null
}

case "$ACTION" in
    mount)
        mkdir -p "$MP"
        mount -o ro "$DEV" "$MP"
        if ! link_valid; then
            rm -f "$LINK" 2>/dev/null || true
            ln -s "$MP" "$LINK"
        fi
        ;;
    umount)
        umount "$MP" 2>/dev/null || true
        rmdir "$MP" 2>/dev/null || true
        if [ -L "$LINK" ] && [ "$(readlink "$LINK")" = "$MP" ]; then
            rm -f "$LINK"
            for other in /mnt/usb-*; do
                if mountpoint -q "$other" 2>/dev/null; then
                    ln -s "$other" "$LINK"
                    break
                fi
            done
        fi
        ;;
    *)
        echo "usage: $0 mount|umount <kernel-device-name>" >&2
        exit 1
        ;;
esac
