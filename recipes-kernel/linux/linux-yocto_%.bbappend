FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " \
    file://amdgpu-enable.cfg \
    file://input-hid-enable.cfg \
    file://disable-unused-subsystems.cfg \
    file://exfat-enable.cfg \
    file://squashfs-boot.cfg \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wifi', 'file://wifi-enable.cfg', '', d)} \
"

# Kernel mapping for this layer's own generic machines. genericx86-64 is
# already in oe-core's COMPATIBLE_MACHINE and just needs its KMACHINE;
# genericarm64 builds from oe-core's standard/base branch and SRCREV, so
# a kernel bump in oe-core needs no change here.
COMPATIBLE_MACHINE:genericarm64 = "genericarm64"
COMPATIBLE_MACHINE:genericx86-64 = "genericx86-64"

# KMACHINE picks the BSP description inside linux-yocto's own
# kernel-cache (no extra layer involved either way). meta-yocto-bsp's
# genericx86-64 uses "common-pc-64"; this layer used that too at first,
# and the very first boot on the Intel desktop this project is tested
# on regressed against the previous meta-intel build in three ways at
# once: WiFi (RTL8852BE) never came up, the installer GUI appeared only
# after ~30 s on a text console, and the USB mouse was dead. The two
# initramfs images were otherwise the same (package lists, scripts and
# units diffed identically, only intel-microcode and the i915 firmware
# were gone) - the kernel config was the only real change. Comparing
# the configs extracted from both bzImages (scripts/extract-ikconfig)
# shows what "common-pc-64" drops relative to meta-intel's kernel: the
# whole Intel platform glue - pinctrl-intel with all PCH generations
# (the GPIO controller behind the ACPI GeneralPurposeIo OpRegion that
# firmware uses for slot power/reset sequencing), MFD_INTEL_LPSS +
# DesignWare I2C/UART/SPI, the DMA engines, xhci-plat/dwc3/typec, TPM,
# and it flips INTEL_MEI from a never-installed module to built-in.
# Rather than guess which of those is the culprit (each test round is
# a real reboot on real hardware), go back to the exact kernel BSP the
# working image had: "intel-corei7-64" is the kernel-cache's
# bsp/intel-common description meta-intel maps its intel-corei7-64 /
# x86-64-v3-intel-common tunes to (see meta-intel's
# meta-intel-compat-kernel.inc), and it is what already booted fine on
# both the AMD and the Intel test machines together with the fragments
# in files/. Despite its name it is not Intel-only - the AMD laptop
# ran it with just amdgpu-enable.cfg on top. Slimming the kernel back
# down can be retried later in small, individually tested steps.
KMACHINE:genericx86-64 ?= "intel-corei7-64"
