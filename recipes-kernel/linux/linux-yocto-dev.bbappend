FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " \
    file://amdgpu-enable.cfg \
    file://input-hid-enable.cfg \
    file://disable-unused-subsystems.cfg \
    file://exfat-enable.cfg \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wifi', 'file://wifi-enable.cfg', '', d)} \
"

KBRANCH:genericx86-64  = "standard/base"

KMACHINE:genericarm64 ?= "genericarm64"
# Same BSP choice as linux-yocto_%.bbappend, see the comment there.
KMACHINE:genericx86-64 ?= "intel-corei7-64"

COMPATIBLE_MACHINE:genericarm64 = "genericarm64"
COMPATIBLE_MACHINE:genericx86-64 = "genericx86-64"
