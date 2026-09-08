# RAM-resident installer/rescue image (raylib/raygui, DRM software
# rendering) - boots directly into an initramfs (unpacked into tmpfs
# by the kernel) instead of a disk partition, so writing to (or
# completely reformatting) ANY disk is safe, including the one
# originally booted from. The "exclude the running system's own boot
# disk" safety check in the app itself remains in place regardless
# (defense in depth).
#
# DELIBERATE FULL PARITY with core-image-installer-raylib.bb (the
# disk-backed variant) as of this rewrite - systemd, packagegroup-
# core-boot, the same IMAGE_INSTALL list, all shared. This replaced an
# earlier version of this same image that used a hand-written /init
# shell script (busybox as PID 1, no systemd at all) and an explicit,
# hand-curated PACKAGE_INSTALL list - reconsidered and abandoned on
# request: maintaining a genuinely different toolchain/init system for
# this one image variant amounted to a de facto second product, not
# just an alternate IMAGE_FSTYPES of the same one - and the bulk of
# this project's own real size-reduction work (trimming to only the
# actually-needed GPU/WiFi firmware) already captured most of the
# space savings the old, minimal-package approach was chasing in the
# first place, removing the strongest remaining argument for keeping
# the two variants structurally different.
#
# ONE deliberate exception to that "full parity" rule: storage-
# partition-helper (mounts a local build-payload partition at /mnt/
# storage on the disk-backed image) is NOT included here - explicitly
# removed on request. It targets a scenario (a dedicated local-payload
# partition living alongside a full desktop install) that doesn't
# apply to a RAM-resident rescue image the same way, brings its own
# extra dependencies along for no benefit in this context, and
# shouldn't run here at all as a matter of scope, not just size.
#
# systemd running directly as PID 1 in a plain initramfs with no
# subsequent switch_root to a real disk (this tmpfs IS the permanent
# root here, nothing to hand off to) is less common than the usual
# transitional-initrd-then-switch_root pattern, but a supported,
# documented systemd mode - confirmed directly against the real
# oe-core systemd recipe/packaging: no reference to /etc/initrd-release
# anywhere in it, so nothing here causes systemd to mistake this image
# for a traditional dracut-style transitional initrd expecting a later
# switch_root (systemd checks for that marker file at runtime to
# decide its own boot target, and without it present, it boots through
# its normal sysinit/basic/multi-user targets same as on a real disk).
# STILL NOT YET VERIFIED ON REAL HARDWARE as of this rewrite - the
# previous /init-script-based version of this image had already gotten
# real boot-test verification (see this layer's own README history);
# this systemd-based replacement has not yet had its own real-hardware
# confirmation that the boot sequence actually completes this way.
#
# USB flash drive auto-mounting (a capability the old /init script
# used to provide, entirely on its own, since there was no udev
# running to do it any other way) is now handled by the new,
# standalone usb-automount package instead (included in IMAGE_INSTALL
# below) - a udev rule + systemd template service, applying equally to
# BOTH image variants now rather than being specific to this one.
SUMMARY = "Installer/rescue image (raylib/raygui, DRM software rendering), boots from initramfs, no disk-backed rootfs"
LICENSE = "BSD-2-Clause"

inherit core-image

# The whole point of this variant - see the comments above.
IMAGE_FSTYPES = "${INITRAMFS_FSTYPES}"

# oe-core's own default INITRAMFS_MAXSIZE (128M) is a safety check -
# the guidance is "should be less than 1/2 of RAM size", not a hard
# technical ceiling. Raised to 512M - see core-image-installer-raylib.bb's
# own firmware comments for the same reasoning on why GPU/WiFi
# firmware sizing drove this number; kept in sync with that image's
# own package list (with the firmware trimming since applied there,
# actual measured size should be well under this ceiling with real
# headroom to spare, not just barely fitting).
INITRAMFS_MAXSIZE = "524288"

# Same IMAGE_INSTALL list as core-image-installer-raylib.bb - see this
# recipe's own top comment on why these two are now deliberately kept
# in lockstep. WKS_FILE/partition-table-building tools (sfdisk/partx)
# deliberately NOT carried over even so - nothing in this image writes
# a partition TABLE to itself (there's no "itself" to partition, this
# boots straight into RAM), only ever to a TARGET disk via the app's
# own dd/tar-extract logic, same division of responsibility the disk-
# backed image already has (see its own comments on why parted/sfdisk-
# adjacent tooling is scoped to storage-partition-helper, not the app).
IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    libdrm \
    bash \
    util-linux-lsblk \
    util-linux-findmnt \
    util-linux-blkid \
    util-linux-mount \
    util-linux-mountpoint \
    sed \
    grep \
    tar \
    gzip \
    curl \
    yocto-rootfs-updater-raylib \
    initramfs-home-mount \
    usb-automount \
    linux-firmware-amdgpu-license \
    linux-firmware-amdgpu-rembrandt \
    linux-firmware-i915-license \
    linux-firmware-i915-dg2 \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wifi', 'iwd linux-firmware-iwlwifi-cc linux-firmware-rtl8852', '', d)} \
    "

# Same debug-console access as the disk-backed image, same reasoning
# (see that recipe's own comment) - now meaningfully usable here too,
# since this variant has a real, working login/getty stack via systemd
# now, unlike the old /init-script version.
IMAGE_FEATURES += "allow-empty-password allow-root-login empty-root-password"

# Deliberately NOT set here, unlike the disk-backed image:
# IMAGE_ROOTFS_SIZE/IMAGE_ROOTFS_EXTRA_SPACE size a real, persistent
# rootfs PARTITION - meaningless for an initramfs (no partition of its
# own at all, RAM-resident only). INITRAMFS_MAXSIZE above is this
# variant's own equivalent size governance instead.

# Do not pollute the initramfs with rootfs features not relevant here
# (same reasoning as oe-core's own core-image-minimal-initramfs.bb).
IMAGE_LINGUAS = ""
