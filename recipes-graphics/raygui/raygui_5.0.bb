SUMMARY = "raygui - immediate-mode GUI library for raylib"
HOMEPAGE = "https://github.com/raysan5/raygui"
LICENSE = "zlib-acknowledgement"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e2f0c68c4ba013e1d4e1fcf68d43927a"

SRC_URI = "git://github.com/raysan5/raygui.git;protocol=https;nobranch=1;tag=${PV}"
SRCREV = "020a61bebcbe288b4414de3416e219ef40af847a"

inherit allarch

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/src/raygui.h ${D}${includedir}/raygui.h
}

FILES:${PN} = ""
FILES:${PN}-dev += "${includedir}/raygui.h"
ALLOW_EMPTY:${PN} = "1"

BBCLASSEXTEND = "native nativesdk"
