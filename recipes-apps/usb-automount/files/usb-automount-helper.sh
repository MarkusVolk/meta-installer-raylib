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
# Mounted read-write, falling back to read-only when the filesystem
# refuses (write-protected media, an unclean NTFS/exFAT volume): a
# stick is also the place to drop logs or a config onto, not just a
# payload source.
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
        # The disk this system booted from is a USB device too, so the udev
        # rule fires for its own partitions as well. Its rootfs and the
        # localstor partition storage-partition-helper handles are already
        # mounted by the time this runs, and mounting them a second time just
        # fails, leaving a misleading "Failed to start Auto-mount USB device
        # sda3" on the console that reads like the storage partition was
        # unavailable. Partitions that are not in use - the boot stick's own
        # ESP among them - are still picked up as before.
        IN_USE=$(findmnt -rno TARGET --source "$DEV" 2>/dev/null | head -1)
        if [ -n "$IN_USE" ]; then
            echo "I: $DEV is already mounted at $IN_USE - not mounting it again."
            exit 0
        fi
        mkdir -p "$MP"
        if ! mount "$DEV" "$MP"; then
            echo "I: read-write mount of $DEV failed - falling back to read-only."
            mount -o ro "$DEV" "$MP"
        fi
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
