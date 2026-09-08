SUMMARY = "GUI tool (native, raylib/raygui, PLATFORM_DRM) to install a rootfs tarball"
DESCRIPTION = "Renders via raylib's PLATFORM_DRM backend directly on /dev/dri/cardX \
(no X11/Wayland). No libcurl/OpenSSL/JSON dependencies: mount, umount, lsblk, \
tar, curl, sha256sum are invoked as system tools."
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

SRC_URI = "\
    file://main.cpp \
    file://backend.hpp \
    file://CMakeLists.txt \
    file://yocto-rootfs-updater-raylib.service \
    file://config.toml \
"

S = "${UNPACKDIR}"

DEPENDS = "raylib raygui libdrm"

# xf86drm.h's own #include <drm.h> needs ${includedir}/libdrm in the
# search path (upstream libdrm layout, verified in raylib_6.0.bb).
CFLAGS:append = " -I${STAGING_INCDIR}/libdrm"
CXXFLAGS:append = " -I${STAGING_INCDIR}/libdrm"

RDEPENDS:${PN} = "\
    util-linux-lsblk \
    util-linux-mount \
    util-linux-umount \
    tar \
    gzip \
    curl \
    coreutils \
    liberation-fonts \
    btrfs-tools \
"

inherit cmake systemd pkgconfig

SYSTEMD_SERVICE:${PN} = "yocto-rootfs-updater-raylib.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/yocto-rootfs-updater-raylib.service ${D}${systemd_system_unitdir}/
    install -d ${D}${sysconfdir}/yocto-rootfs-updater-raylib
    install -m 0644 ${UNPACKDIR}/config.toml ${D}${sysconfdir}/yocto-rootfs-updater-raylib/config.toml
}

FILES:${PN} += "\
    ${systemd_system_unitdir}/yocto-rootfs-updater-raylib.service \
    ${sysconfdir}/yocto-rootfs-updater-raylib/config.toml \
"
CONFFILES:${PN} += "${sysconfdir}/yocto-rootfs-updater-raylib/config.toml"
