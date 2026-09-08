SUMMARY = "Create/mount a local storage partition in unused trailing disk space"
DESCRIPTION = "Runs at boot of the rescue/installer system itself (not the target \
being installed): if the boot disk has >=32GiB unused trailing space (e.g. a wic \
image dd'd onto larger media), creates and mounts an ext4 partition there at \
/mnt/storage, so large local rootfs.tar.gz/.wic files have somewhere to live \
before being selected via 'Lokale Datei' in the installer. Not needed for HTTPS \
installs, which stream directly. Idempotent: does nothing but mount if the \
partition already exists from a previous boot."
SECTION = "admin"

LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

SRC_URI = " \
    file://storage-partition-helper.service \
    file://storage-partition-helper \
"

S = "${UNPACKDIR}"

inherit systemd

RDEPENDS:${PN} += "util-linux-sfdisk util-linux-partx e2fsprogs-mke2fs util-linux-blkid util-linux-findmnt util-linux-lsblk util-linux-mount util-linux-mountpoint coreutils sed grep"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/storage-partition-helper.service ${D}${systemd_system_unitdir}
    install -d ${D}${sbindir}
    install -m 0755 ${UNPACKDIR}/storage-partition-helper ${D}${sbindir}
}

SYSTEMD_SERVICE:${PN} = "storage-partition-helper.service"
