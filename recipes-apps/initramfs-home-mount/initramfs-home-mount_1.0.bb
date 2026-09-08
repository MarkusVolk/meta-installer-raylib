SUMMARY = "Mounts a home-labeled partition on the boot disk at /mnt/storage (initramfs image only)"
DESCRIPTION = "The initramfs image's own, much smaller replacement for \
storage-partition-helper - only finds and mounts an ALREADY-existing \
\"home\"-labeled partition on the same disk this system itself booted \
from, no partition creation/growing logic at all (that part was \
explicitly dropped as unnecessary here). Determines the boot disk via \
systemd-boot's own LoaderDevicePartUUID EFI variable, not the root \
filesystem's own backing device (meaningless here - this image's own \
root is a tmpfs, with no real block device backing it at all)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://initramfs-home-mount \
           file://initramfs-home-mount.service \
"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "initramfs-home-mount.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "util-linux-blkid util-linux-lsblk util-linux-mount coreutils"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/initramfs-home-mount ${D}${bindir}/initramfs-home-mount

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/initramfs-home-mount.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} = " \
    ${bindir}/initramfs-home-mount \
    ${systemd_system_unitdir}/initramfs-home-mount.service \
"
