include conf/machine/include/x86/x86-base.inc
require conf/machine/include/x86/qemuboot-x86.inc
MACHINE_FEATURES += "wifi efi pcbios"

PREFERRED_PROVIDER_virtual/kernel ?= "linux-yocto"

MACHINE_EXTRA_RRECOMMENDS += "kernel-modules linux-firmware"
# What meta-intel's machine include would otherwise recommend for the
# same hardware class: the Intel display driver, the two NIC drivers most
# commonly found in current x86 machines, and the r8169 firmware blobs.
# Recommendations, so a kernel that builds any of these in (=y) is fine.
MACHINE_ESSENTIAL_EXTRA_RRECOMMENDS += "kernel-module-i915 kernel-module-igc kernel-module-r8152 linux-firmware-rtl-nic"
# meta-intel's machine include also force-loads these two at boot via
# modules-load.d (its INTEL_COMMON_AUTOLOAD, minus "uio" which this
# layer never builds) instead of leaving them to udev's coldplug alone.
# That is how the previously working image behaved - i915 was bound
# before the installer app ever opened the DRM device, rather than
# simpledrm handing over to i915 somewhere during boot - so keep it.
# Harmless when a module is not built or not installed: the
# modules-load.d snippet lives inside the respective kernel-module
# package.
KERNEL_MODULE_AUTOLOAD += "i915 iwlwifi"

IMAGE_FSTYPES += "wic.zst wic.bmap"
WKS_FILE ?= "genericx86.wks.in"
EFI_PROVIDER ??= "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'systemd-boot', 'grub-efi', d)}"
do_image_wic[recrdeptask] += "do_bootimg"
