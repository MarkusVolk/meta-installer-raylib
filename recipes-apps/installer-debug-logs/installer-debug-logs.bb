SUMMARY = "Debug helper: collects input/display/WiFi diagnostics onto a USB stick"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

SRC_URI = "file://collect-logs.sh"

S = "${UNPACKDIR}"

# Every external tool the script calls, cross-checked against the
# script itself (same lesson as in core-image-installer-raylib.bb:
# don't discover missing tools one at a time on real hardware).
# journalctl/udevadm/systemctl come with systemd/udev via
# packagegroup-core-boot; bash for "read -t", "$(( ))" and process
# substitution; coreutils (+ its stdbuf split-off) for timeout, dd,
# od, stdbuf, seq, tr, sort, wc, sync, date, readlink, stty.
RDEPENDS:${PN} = "\
    bash \
    coreutils \
    coreutils-stdbuf \
    util-linux-lsblk \
    util-linux-findmnt \
    util-linux-mount \
    util-linux-umount \
    util-linux-mountpoint \
    tar \
    gzip \
    udev \
"

inherit allarch

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/collect-logs.sh ${D}${bindir}/installer-collect-logs
    install -d ${D}${ROOT_HOME}
    ln -sf ${bindir}/installer-collect-logs ${D}${ROOT_HOME}/collect-logs.sh
}

FILES:${PN} += "${ROOT_HOME}/collect-logs.sh"
