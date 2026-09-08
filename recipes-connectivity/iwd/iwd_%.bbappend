# Standard bbappend idiom - without this, bitbake only searches for
# SRC_URI files relative to the ORIGINAL recipe's own directory (in
# meta-openembedded), never this layer's own files/ directory right
# next to this bbappend. Found via a real build error before this was
# added ("Unable to get checksum for iwd SRC_URI entry main.conf:
# file could not be found"). Points at "files" specifically (not the
# more common "${PN}" convention, i.e. an "iwd/" subdirectory) to
# match this project's own consistent files/ subdirectory naming used
# throughout every other recipe in this same layer.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# iwd's own recipe ships no main.conf at all by default (checked
# directly - its own doc/main.conf example is entirely commented-out/
# opt-in, matching iwd's own documented default of NOT managing
# network configuration on its own). Installs this project's own copy
# instead - see files/main.conf's own comments for the full story
# (EnableNetworkConfiguration/NameResolvingService, found via a real
# user report that WiFi looked connected but plain curl still failed).
#
# iwd-tmpfs-state.conf: a systemd drop-in override (NOT a replacement
# of iwd's own packaged unit file - the standard, least invasive way
# to customize a third-party service's behavior) redirecting iwd's own
# persistent network-profile storage away from the real, non-volatile
# rootfs and onto tmpfs instead - see that file's own comments for the
# full reasoning (found while investigating the passphrase-in-log
# report, a related but separate leak path).
SRC_URI += "file://main.conf file://iwd-tmpfs-state.conf"

do_install:append() {
    install -d ${D}${sysconfdir}/iwd
    install -m 0644 ${UNPACKDIR}/main.conf ${D}${sysconfdir}/iwd/main.conf

    install -d ${D}${systemd_unitdir}/system/iwd.service.d
    install -m 0644 ${UNPACKDIR}/iwd-tmpfs-state.conf ${D}${systemd_unitdir}/system/iwd.service.d/tmpfs-state.conf
}

FILES:${PN} += " \
    ${sysconfdir}/iwd/main.conf \
    ${systemd_unitdir}/system/iwd.service.d/tmpfs-state.conf \
"
CONFFILES:${PN} += "${sysconfdir}/iwd/main.conf"
