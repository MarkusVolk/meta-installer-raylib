# The Pi boots through EDK2 and systemd-boot: the kernel has to be an EFI
# binary, and squashfs-boot mounts the installer squashfs before any module
# could be loaded, which bcm2711_defconfig builds squashfs and overlayfs as.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${THISDIR}/../../../../recipes-kernel/linux/files:"

SRC_URI:append = " \
    file://efi.cfg \
    file://squashfs-boot.cfg \
"
