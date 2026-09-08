SUMMARY = "Auto-mounts USB-attached block device partitions to /mnt/usb-<device>"
DESCRIPTION = "A udev rule triggers a per-device systemd service on \
every USB flash drive partition that appears, mounting it read-only \
at /mnt/usb-<kernel-device-name> - matches this project's own \
existing app-side auto-select logic (main.cpp's \
discover_local_file_candidate_dirs(), any /mnt/ subdirectory whose \
name starts with \"usb\") without needing any app-side changes. \
Replaces the initramfs-only /init script's own USB-mounting logic \
that variant used to have before that image was rebuilt to match the \
disk-backed image's own systemd-based approach - this package now \
covers both."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://99-usb-automount.rules \
           file://usb-automount-helper.sh \
           file://usb-automount@.service \
"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "usb-automount@.service"
# Template unit - nothing to auto-enable directly (there's no single,
# fixed instance name to enable ahead of time), the udev rule's own
# SYSTEMD_WANTS is what actually starts each concrete instance as
# devices appear. Confirmed this is the correct, standard pattern for
# a templated device-triggered service (not an oversight).
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

RDEPENDS:${PN} = "${VIRTUAL-RUNTIME_dev_manager} util-linux-mount util-linux-umount util-linux-mountpoint coreutils"

do_install() {
    install -d ${D}${base_libdir}/udev/rules.d
    install -m 0644 ${UNPACKDIR}/99-usb-automount.rules ${D}${base_libdir}/udev/rules.d/

    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/usb-automount-helper.sh ${D}${bindir}/usb-automount-helper.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/usb-automount@.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} = " \
    ${base_libdir}/udev/rules.d/99-usb-automount.rules \
    ${bindir}/usb-automount-helper.sh \
    ${systemd_system_unitdir}/usb-automount@.service \
"
