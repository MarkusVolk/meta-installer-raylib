# Trimmed Intel DG2 (discrete Arc A-series) GPU firmware, ~400 KB
# instead of the ~27 MB oe-core's own umbrella ${PN}-i915 package
# carries (every GuC/HuC/DMC blob for every Intel GPU generation).
# Same per-GPU trimming idea oe-core already applies to amdgpu
# (${PN}-amdgpu-rembrandt etc.) but never got for i915.
#
# Needed after a real-hardware test with this project's own initramfs
# image on an Arc card (DG2/G10, PCI device 56a0): the distro conf had
# deliberately blocked linux-firmware-i915 via BAD_RECOMMENDATIONS to
# check whether Intel display init works without any blobs at all in
# software-rendering mode. It does not on DG2 - the kernel log showed
# "GuC firmware i915/dg2_guc_70.bin: fetch failed -ENOENT", "Enabling
# uc failed (-5)", "Failed to initialize GPU, declaring it wedged!",
# and raylib's drmModeSetCrtc() then failed with -5 (EIO). On DG2 GuC
# submission is mandatory (no execlists fallback), and framebuffers
# live in the card's own VRAM where allocating/clearing them goes
# through the (now wedged) GT - so even the plain dumb-buffer
# software path can't bring up a display. fbcon only survived because
# its framebuffer was already set up earlier. Integrated Intel GPUs
# up to Alder Lake don't hit this (GuC optional there, framebuffer in
# system memory) - which is why the "no blobs needed" claim looked
# true on the earlier iGPU test.
#
# Exactly the two files the 6.18 kernel asked for in that log:
#   dg2_guc_70.bin      - GuC (mandatory, see above; the unversioned
#                         name is the newest 70.x release, the
#                         versioned dg2_guc_70.1.2/70.4.1 siblings are
#                         older fallbacks the kernel never requested)
#   dg2_dmc_ver2_08.bin - DMC (display microcontroller; its absence
#                         only costs runtime power management, kept
#                         because it's 22 KB and silences the second
#                         firmware error in the same log)
# dg2_huc_gsc.bin (HuC, media encode/decode only, 630 KB) is
# deliberately left out - the kernel treats a missing HuC as non-fatal
# and this image never touches the media engine. Add it to FILES below
# should that ever change.
#
# "=+" prepends, so this package sits ahead of oe-core's own ${PN}-i915
# in PACKAGES and claims its two files first - FILES:${PN}-i915 is the
# whole ${firmwaredir}/i915 directory and would otherwise swallow them.
# The trailing "*" on each glob keeps this working with
# FIRMWARE_COMPRESSION set (files then end in .bin.xz/.bin.zst).
PACKAGES =+ "${PN}-i915-dg2"
LICENSE:${PN}-i915-dg2 = "LicenseRef-Firmware-i915"
FILES:${PN}-i915-dg2 = " \
    ${firmwaredir}/i915/dg2_guc_70.bin* \
    ${firmwaredir}/i915/dg2_dmc_ver2_08.bin* \
"
RDEPENDS:${PN}-i915-dg2 = "${PN}-i915-license"

# Intel Wi-Fi 6 AX200 ("cc" family in linux-firmware's naming) WiFi
# firmware, ~1.3 MB - the single blob the 6.18 kernel asks for.
# Same trimming idea as the DG2 package above, applied to iwlwifi: oe-core
# splits out per-chip packages for the older generations (${PN}-iwlwifi-
# 9260 etc.) but drops every newer one - including all seven
# iwlwifi-cc-a0-{50..77}.ucode revisions - into the ${PN}-iwlwifi-misc
# catch-all (FILES = iwlwifi-*.ucode*, ~100 MB for every current Intel
# card in one package).
#
# This replaced the layer's own former linux-firmware-intel_1.0.bb, which
# fetched the same file by name from a third-party GitHub mirror with its
# own SRC_URI - a second copy of the linux-firmware source just for one
# blob, at a mirror-frozen revision. The blob now comes from the very
# linux-firmware tarball oe-core already fetches for the other firmware
# packages in the image, at whatever revision oe-core currently tracks.
#
# Only iwlwifi-cc-a0-77.ucode: the kernel's 22000-family driver has
# IWL_22000_UCODE_API_MIN == IWL_22000_UCODE_API_MAX == 77
# (drivers/net/wireless/intel/iwlwifi/cfg/22000.c in the 6.18 kernel-
# source tree), so it requests exactly that version and never falls back
# to the older -50/-59/-66/-72/-73/-74 siblings. No .pnvm needed for this
# family either. The file lives under intel/iwlwifi/ in linux-firmware
# with a compatibility symlink at the top level; oe-core's own iwlwifi
# FILES lists both locations and so does this one.
#
# "=+" again, so this package precedes ${PN}-iwlwifi-misc in PACKAGES
# and claims its file before the catch-all glob does; trailing "*" for
# FIRMWARE_COMPRESSION as above.
PACKAGES =+ "${PN}-iwlwifi-cc"
LICENSE:${PN}-iwlwifi-cc = "LicenseRef-Firmware-iwlwifi-firmware"
FILES:${PN}-iwlwifi-cc = " \
    ${firmwaredir}/iwlwifi-cc-a0-77.ucode* \
    ${firmwaredir}/intel/iwlwifi/iwlwifi-cc-a0-77.ucode* \
"
RDEPENDS:${PN}-iwlwifi-cc = "${PN}-iwlwifi-license"
