SUMMARY = "Minimal installer/rescue image (raylib/raygui, DRM software rendering): runs the RootFS Updater"
LICENSE = "BSD-2-Clause"

inherit core-image

# Hard "=", not "?=": meta-intel's intel-corei7-64.conf (MACHINE conf,
# parsed before this recipe) also sets WKS_FILE via "?=" - since that
# runs first, our own weak "?=" here would never win, silently pulling
# in meta-intel's systemd-bootdisk-microcode.wks.in (platform/swap
# layout) instead of ours. Confirmed live: that file is exactly where
# the unexpected "platform"/"swap" labels came from.
WKS_FILE = "installer.wks.in"

IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    libdrm \
    bash \
    util-linux-lsblk \
    util-linux-sfdisk \
    util-linux-partx \
    util-linux-findmnt \
    util-linux-blkid \
    util-linux-mount \
    util-linux-mountpoint \
    e2fsprogs-mke2fs \
    sed \
    grep \
    tar \
    gzip \
    curl \
    yocto-rootfs-updater-raylib \
    storage-partition-helper \
    usb-automount \
    linux-firmware-amdgpu-license \
    linux-firmware-amdgpu-rembrandt \
    linux-firmware-i915-license \
    linux-firmware-i915-dg2 \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wifi', 'iwd linux-firmware-iwlwifi-cc linux-firmware-rtl8852', '', d)} \
    "

# parted deliberately not included: storage-partition-helper switched
# to sfdisk exclusively for partition table work (see its own
# comments) - sfdisk's "-F --bytes"/"size=+" give exact byte/sector
# values throughout, avoiding a round-trip through parted's
# MiB-rounded-for-display text that occasionally produced a start
# value not landing on a valid sector boundary. Found via real-world
# use of the sibling host-side script build-payload-image.sh (same
# technique, applied to an image file instead of a live disk).
#
# util-linux-partx: replaces partprobe (which comes from the parted
# package we just removed) for making the kernel re-read the
# partition table after sfdisk changes it - found the hard way on
# real hardware, storage-partition-helper.service failed with
# "partprobe: command not found".
#
# sed, grep, util-linux-mount, util-linux-mountpoint: also found
# missing on real hardware, one at a time, each after fixing the
# previous ("which", then "partprobe", then "sed"/login-shell error) -
# storage-partition-helper.service kept failing on the next undeclared
# tool. After the third round, went through the whole script and
# cross-checked EVERY external command it calls against RDEPENDS
# instead of continuing to fix these one at a time as they surface on
# real hardware - these two were the remaining gaps. util-linux splits
# mount/mountpoint into their own per-binary packages (confirmed in
# # oe-core's util-linux.bb "binprogs_a" list) - not included by the
# base "util-linux" package above, same as util-linux-umount already
# needed explicitly in the app's own recipe.
#
# gzip: found missing on real hardware too, this time in the APP
# itself (RDEPENDS in yocto-rootfs-updater-raylib_1.0.bb), not
# storage-partition-helper - "tar -xzf" failed to extract a
# rootfs.tar.gz. oe-core's tar recipe has no built-in zlib linking
# (checked its .bb - no PACKAGECONFIG for it), so GNU tar's -z/--gzip
# flag shells out to an external gzip at runtime regardless.

# AMD GPU firmware stays even without opengl/mesa: amdgpu needs it for
# basic display init/modesetting itself, not just for accelerated
# rendering. libgbm/libegl-mesa/mesa-megadriver removed again along
# with opengl (see installer-minimal-raylib.conf) - software rendering
# doesn't need them. TESTABLE HYPOTHESIS, not yet confirmed on real
# AMD hardware: please verify the display still works correctly with
# this simpler config before relying on it.
#
# Intel DG2 (discrete Arc) firmware, same story: linux-firmware-i915-dg2
# is this layer's own ~400 KB trimmed package (recipes-kernel/
# linux-firmware/linux-firmware_%.bbappend, GuC + DMC only) rather
# than oe-core's ~27 MB umbrella linux-firmware-i915, which the distro
# conf keeps blocked. Confirmed necessary on real hardware: without
# the GuC blob a DG2 card's GT comes up "wedged" and drmModeSetCrtc()
# fails with -5 (EIO) even for pure software rendering - see the
# bbappend's own comment for the full log trail. Integrated Intel
# GPUs don't need it and don't get it from this package either; add
# the matching blobs for your own GPU the same way if a different
# Intel generation ever turns out to need them.

# Local console debug access (Ctrl+Alt+F2, empty root password) -
# no dropbear/SSH though, that stays out. Kept after reconsidering:
# without any access at all, the only recourse for future debugging
# would be mounting this rootfs from another system - console login
# is the far more practical fallback for a physically-present device.
IMAGE_FEATURES += "allow-empty-password allow-root-login empty-root-password"

IMAGE_ROOTFS_SIZE ?= "524288"
IMAGE_ROOTFS_EXTRA_SPACE:append = " + 262144"
