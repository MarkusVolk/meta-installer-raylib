#!/bin/sh
# Host-side tool (run on the build machine, NOT part of the target/
# rescue image itself): takes an already-built .wic image plus one or
# more payload files (rootfs.tar.gz, a .wic image to install, etc.)
# and produces a combined image with an extra, already-populated ext4
# partition holding those files - ready to flash onto a USB stick in
# one step, no "boot the installer, then scp files over" detour needed.
#
# Same GPT-relocate + free-space-append technique as
# storage-partition-helper, applied to an image FILE on the build host
# instead of a live system's own boot disk. sfdisk exclusively (no
# parted): "size=+" appends with exact sector values, avoiding
# parted's MiB-rounded "print free" text, which occasionally produced
# a start value off a valid sector boundary.
#
# Directory settings (payload_output_dir, payload_source_dir) are
# NOT shared by name with the target's own config.toml (unlike
# kernel_target_name below, which genuinely is the same kind of value -
# a bare filename - in both places): these are host-side absolute
# paths, a different shape from config.toml's own local_default_dir
# (a relative subdirectory name under /mnt/storage on the target) -
# same name would suggest the values are interchangeable when they
# aren't.
#
# All arguments optional, can run with zero. Config via env vars or
# ~/.config/build-payload-image.conf - see build-payload-image.conf.example.
# Usage: build-payload-image.sh [-i input.wic] [-o output.img] [-p payload-file]...

set -e

# sudo resets $HOME to root's home by default (unless "sudo -E") -
# since this script always needs root, that would silently miss the
# invoking user's own config every time. Resolve their real home via
# $SUDO_USER instead, falling back to plain $HOME if not running
# under sudo or if getent isn't available.
REAL_HOME="$HOME"
if [ -n "$SUDO_USER" ] && command -v getent >/dev/null 2>&1; then
    SUDO_USER_HOME=$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)
    [ -n "$SUDO_USER_HOME" ] && REAL_HOME="$SUDO_USER_HOME"
fi
CONFIG_FILE="${BUILD_PAYLOAD_IMAGE_CONFIG:-$REAL_HOME/.config/build-payload-image.conf}"
[ -f "$CONFIG_FILE" ] && . "$CONFIG_FILE"

usage() {
    echo "Usage: $0 [-i input.wic] [-o output.img] [-p payload-file]..." >&2
    echo "  All arguments optional - can run with none at all. Repeat -p for" >&2
    echo "  multiple payload files. No positional arguments accepted." >&2
    echo "  Without -i: auto-detects the *.wic symlink under" >&2
    echo "  tmp/deploy/images/*/ relative to the current directory" >&2
    echo "  (run from your build dir). If \$MACHINE is set, that machine's" >&2
    echo "  deploy dir is tried first, resolving what would otherwise be" >&2
    echo "  ambiguous." >&2
    echo "  -i <name>: bare filename also resolves under tmp/deploy/images/*/." >&2
    echo "  Without -o: derived from the input filename as" >&2
    echo "  '<stem>-combined.<ext>'." >&2
    echo "  payload_output_dir (env or ~/.config/build-payload-image.conf)" >&2
    echo "  resolves a bare -o filename - see script header." >&2
    echo "  Without -p: payload_source_dir auto-discovers" >&2
    echo "  *.rootfs.tar.gz / *.wic symlinks there instead." >&2
    exit 1
}

INPUT_ARG=""
OUTPUT_ARG=""
PAYLOAD_ARGS=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -i|--input)
            [ "$#" -ge 2 ] || usage
            INPUT_ARG="$2"
            shift 2
            ;;
        -o|--output)
            [ "$#" -ge 2 ] || usage
            OUTPUT_ARG="$2"
            shift 2
            ;;
        -p|--payload)
            [ "$#" -ge 2 ] || usage
            # Space-separated accumulation - not fully space-in-path
            # safe, but Yocto build artifacts never have spaces in
            # their names.
            PAYLOAD_ARGS="$PAYLOAD_ARGS $2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "E: Unknown/unexpected argument: $1 (only -i/-o/-p flags are accepted, no positional arguments)" >&2
            usage
            ;;
    esac
done

resolve_path() {
    # $1 = given path, $2 = directory to resolve against if $1 has no "/"
    case "$1" in
        */*) echo "$1" ;;
        *)   if [ -n "$2" ]; then echo "${2%/}/$1"; else echo "$1"; fi ;;
    esac
}

# Bare filename for -i/--input resolves against the currently built
# image (tmp/deploy/images/*/<filename>), never a fixed configured
# directory - always "whatever was just built here". No argument at
# all auto-detects the lone *.wic symlink there.
resolve_deploy_input() {
    if [ -z "$1" ]; then
        MATCHES=""
        # If $MACHINE is set (bitbake's own variable name), try that
        # machine's deploy dir first - avoids ambiguity from leftover
        # test artifacts alongside your real target.
        if [ -n "$MACHINE" ]; then
            for f in "tmp/deploy/images/${MACHINE}"/*.wic; do
                [ -e "$f" ] && [ -L "$f" ] && MATCHES="$MATCHES $f"
            done
        fi
        if [ -z "$MATCHES" ]; then
            for f in tmp/deploy/images/*/*.wic; do
                [ -e "$f" ] && [ -L "$f" ] && MATCHES="$MATCHES $f"
            done
        fi
        set -- $MATCHES
        case "$#" in
            0) echo "E: No *.wic symlink found under tmp/deploy/images/*/ (in the current directory) - are you in your build directory, or do you need -i?" >&2
               exit 1 ;;
            1) echo "$1" ;;
            *) echo "E: Ambiguous, multiple *.wic symlinks under tmp/deploy/images/*/: $* - pass -i with the desired filename, or export MACHINE to disambiguate automatically." >&2
               exit 1 ;;
        esac
        return
    fi
    case "$1" in
        */*) echo "$1"; return ;;
    esac
    WANTED="$1"
    MATCHES=""
    for f in tmp/deploy/images/*/"$WANTED"; do
        [ -e "$f" ] && MATCHES="$MATCHES $f"
    done
    set -- $MATCHES
    case "$#" in
        0) echo "E: '$WANTED' not found under tmp/deploy/images/*/ (in the current directory) - are you in your build directory?" >&2
           exit 1 ;;
        1) echo "$1" ;;
        *) echo "E: Ambiguous, multiple matches under tmp/deploy/images/*/: $*" >&2
           exit 1 ;;
    esac
}

INPUT_WIC=$(resolve_deploy_input "$INPUT_ARG")

# Default output location, if payload_output_dir isn't set: the same
# directory the input .wic lives in (its tmp/deploy/images/<machine>/),
# not the current working directory - keeps the combined image next
# to the build's other artifacts by default, which is normally what
# you want. payload_output_dir still overrides this when set.
OUTPUT_DEFAULT_DIR="${payload_output_dir:-$(dirname "$INPUT_WIC")}"

if [ -n "$OUTPUT_ARG" ]; then
    OUTPUT_IMG=$(resolve_path "$OUTPUT_ARG" "$OUTPUT_DEFAULT_DIR")
else
    # No -o given either: derive a name from the input file -
    # "<stem>-combined.<ext>" - and resolve it the same way a bare
    # filename would. Together with input auto-detection and
    # payload_source_dir payload auto-discovery, this lets the whole
    # script run with zero arguments.
    BASENAME=$(basename "$INPUT_WIC")
    case "$BASENAME" in
        *.*) EXT=".${BASENAME##*.}"; STEM="${BASENAME%.*}" ;;
        *)   EXT=""; STEM="$BASENAME" ;;
    esac
    OUTPUT_IMG=$(resolve_path "${STEM}-combined${EXT}" "$OUTPUT_DEFAULT_DIR")
    echo "No output filename given, using automatically: ${OUTPUT_IMG}"
fi

# No -p given at all: try to auto-discover payload files via Yocto's
# stable deploy symlinks (always point at the latest build, never the
# timestamped actual files - avoids picking up old builds or
# duplicating symlink+target).
if [ -n "$PAYLOAD_ARGS" ]; then
    set -- $PAYLOAD_ARGS
else
    [ -n "$payload_source_dir" ] || {
        echo "E: No payload file given and payload_source_dir not set."
        usage
    }
    [ -d "$payload_source_dir" ] || {
        echo "E: payload_source_dir not found: $payload_source_dir"
        PARENT_DIR=$(dirname "$payload_source_dir")
        if [ -d "$PARENT_DIR" ]; then
            echo "   Available machine directories under ${PARENT_DIR}:"
            for d in "$PARENT_DIR"/*/; do
                [ -d "$d" ] && echo "     ${d}"
            done
        fi
        exit 1
    }
    echo "No payload file given, searching in ${payload_source_dir}..."
    FOUND=""
    for pattern in "*.rootfs.tar.gz" "*.wic"; do
        for f in "$payload_source_dir"/$pattern; do
            [ -e "$f" ] && [ -L "$f" ] && FOUND="$FOUND $f"
        done
    done
    # Kernel image not included by default - unlike rootfs.tar.gz/.wic,
    # there's no reliable glob pattern for it (filenames vary by
    # architecture). Opt in via kernel_target_name with the exact
    # stable deploy symlink name for your MACHINE (see header comment
    # on why this name is shared with the target's own config.toml).
    # Typically "bzImage" for x86.
    if [ -n "$kernel_target_name" ]; then
        KF="$payload_source_dir/$kernel_target_name"
        if [ -e "$KF" ] && [ -L "$KF" ]; then
            FOUND="$FOUND $KF"
        else
            echo "W: kernel_target_name set to '$kernel_target_name' but no such symlink found in ${payload_source_dir} - skipping kernel."
        fi
    fi
    if [ -z "$FOUND" ]; then
        echo "E: No *.rootfs.tar.gz/*.wic symlinks found in ${payload_source_dir}."
        exit 1
    fi
    echo "Automatically found:"
    for f in $FOUND; do echo "  $f (-> $(readlink "$f"))"; done
    set -- $FOUND
fi

[ "$(id -u)" = "0" ] || { echo "E: Must run as root (loop devices, mount)."; exit 1; }

[ -f "$INPUT_WIC" ] || { echo "E: Input image not found: $INPUT_WIC"; exit 1; }
for f in "$@"; do
    [ -f "$f" ] || { echo "E: Payload file not found: $f"; exit 1; }
done

for tool in sfdisk partx mkfs.ext4 losetup truncate; do
    command -v "$tool" >/dev/null 2>&1 || { echo "E: Need $tool"; exit 1; }
done

PAYLOAD_BYTES=0
for f in "$@"; do
    SZ=$(stat -L -c %s "$f")
    PAYLOAD_BYTES=$((PAYLOAD_BYTES + SZ))
done

# 10% headroom + 512MiB fixed buffer (ext4 metadata/journal/reserved
# blocks, plus a little slack for future files).
EXTRA_MIB=$(( (PAYLOAD_BYTES / 1024 / 1024) * 110 / 100 + 512 ))
echo "Total payload: $((PAYLOAD_BYTES / 1024 / 1024))MiB, new partition: ${EXTRA_MIB}MiB"

echo "Copying ${INPUT_WIC} to ${OUTPUT_IMG}..."
OUTPUT_DIR="$(dirname "$OUTPUT_IMG")"
mkdir -p "$OUTPUT_DIR"
# world-writable + sticky bit (like /tmp): anyone can write here, but
# only each file's own owner (or root) can delete/rename it.
chmod 1777 "$OUTPUT_DIR" 2>/dev/null || true
cp --sparse=always "$INPUT_WIC" "$OUTPUT_IMG"

CURRENT_SIZE=$(stat -c %s "$OUTPUT_IMG")
NEW_SIZE=$((CURRENT_SIZE + EXTRA_MIB * 1024 * 1024))
truncate -s "$NEW_SIZE" "$OUTPUT_IMG"

LOOPDEV=$(losetup -f)
losetup -P "$LOOPDEV" "$OUTPUT_IMG"
cleanup() { losetup -d "$LOOPDEV" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# Make the newly appended space visible to sfdisk (same reasoning as
# storage-partition-helper: growing the file doesn't update the GPT).
# --no-reread: same reasoning as storage-partition-helper - newer
# sfdisk refuses by default to touch a disk it considers "in use".
# Shouldn't normally trigger here (a loop-backed file, not a live
# mounted disk), but harmless to include for consistency/safety.
sfdisk --no-reread --relocate gpt-bak-std "$LOOPDEV"
partx -u "$LOOPDEV"

echo "Creating payload partition..."
echo "size=+, type=L" | sfdisk --no-reread -a "$LOOPDEV"
partx -u "$LOOPDEV"

NEWPART=$(sfdisk -d "$LOOPDEV" | grep '^/dev/' | tail -n 1 | cut -d: -f1 | tr -d ' ')
[ -b "$NEWPART" ] || { echo "E: New partition ${NEWPART} not found after creation."; exit 1; }

echo "Formatting ${NEWPART} as ext4 (label localstor)..."
mkfs.ext4 -F -L localstor "$NEWPART"

MNT=$(mktemp -d)
mount "$NEWPART" "$MNT"
# 1777 (sticky bit, like /tmp): lets a regular desktop user write
# directly to this partition later too, not just the rescue system's
# own root-run app.
chmod 1777 "$MNT"
echo "Copying payload files..."
for f in "$@"; do
    cp -v "$f" "$MNT/"
done
sync
umount "$MNT"
rmdir "$MNT"

# Detach explicitly (not just via the EXIT trap) so the image file is
# fully flushed/closed before bmaptool reads it.
losetup -d "$LOOPDEV"
trap - EXIT INT TERM

if command -v bmaptool >/dev/null 2>&1; then
    # The ORIGINAL input.wic's own .bmap (if any) no longer matches
    # (layout changed) - generate a fresh one instead.
    echo "Creating ${OUTPUT_IMG}.bmap..."
    bmaptool create "$OUTPUT_IMG" -o "${OUTPUT_IMG}.bmap"
else
    echo "Note: bmaptool not found - no .bmap created. Any .bmap from the"
    echo "original input image no longer matches (different layout/size) -"
    echo "use a plain dd or 'bmaptool copy' without a .bmap (slower full copy)."
fi

echo "Done: ${OUTPUT_IMG}"

# Hand ownership back to the invoking user - running as root/sudo
# would otherwise leave the file owned by root.
if [ -n "$SUDO_USER" ]; then
    SUDO_USER_GROUP=$(id -gn "$SUDO_USER" 2>/dev/null || true)
    if [ -n "$SUDO_USER_GROUP" ]; then
        chown "${SUDO_USER}:${SUDO_USER_GROUP}" "$OUTPUT_IMG" 2>/dev/null || true
        [ -f "${OUTPUT_IMG}.bmap" ] && chown "${SUDO_USER}:${SUDO_USER_GROUP}" "${OUTPUT_IMG}.bmap" 2>/dev/null || true
    fi
fi

echo "Ready to flash, e.g.: bmaptool copy ${OUTPUT_IMG} /dev/sdX"
