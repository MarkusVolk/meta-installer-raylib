SUMMARY = "raylib is a simple and easy-to-use library to enjoy videogames programming"
HOMEPAGE = "https://github.com/raysan5/raylib"
LICENSE = "zlib-acknowledgement"
LIC_FILES_CHKSUM = "file://LICENSE;md5=4f718fed396a1bda683c98481e5b57a8"

SRC_URI = "\
    git://github.com/raysan5/raylib.git;protocol=https;nobranch=1;tag=${PV} \
    file://0001-DRM-poll-all-keyboard-classified-evdev-devices.patch \
    file://0002-DRM-apply-shift-to-charPressedQueue.patch \
    file://0003-DRM-poll-all-mouse-classified-evdev-devices.patch \
    file://0004-DRM-detect-input-devices-plugged-in-after-startup.patch \
    file://0005-DRM-do-not-sum-relative-movement-across-multiple-mo.patch \
    file://0006-software-fix-scissor-Y-double-flip-causing-scissore.patch \
"
SRCREV = "dbc56a87da87d973a9c5baa4e7438a9d20121d28"

inherit cmake

PACKAGECONFIG ??= "drm"
PACKAGECONFIG[drm] = "-DPLATFORM=DRM,,libdrm"
PACKAGECONFIG[x11] = "-DPLATFORM=Desktop,,virtual/libx11 libxext libxrandr libxinerama libxcursor libxi"
PACKAGECONFIG[opengl] = "-DOPENGL_VERSION='ES 2.0',-DOPENGL_VERSION=Software,mesa virtual/egl virtual/libgles2"
# Deliberately a separate :append line, not merged into the ??= above:
# ??= is a WEAK default - if a downstream layer overrides PACKAGECONFIG
# with a stronger operator (e.g. plain "=", or a bbappend), the
# DISTRO_FEATURES-driven opengl filter would silently disappear along
# with it. A separate :append always applies regardless of how the
# base value was set, and can still be cleanly undone downstream with
# PACKAGECONFIG:remove = "opengl" if genuinely not wanted.
PACKAGECONFIG:append = " ${@bb.utils.filter('DISTRO_FEATURES', 'opengl', d)}"

EXTRA_OECMAKE += " \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
"

CFLAGS:append = " -I${STAGING_INCDIR}/libdrm"
