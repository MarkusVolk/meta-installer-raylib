FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Explicitly volatile journal storage (RAM-only) rather than relying
# on systemd's own "auto" default - see files/10-storage-volatile.conf
# itself for the full reasoning (found during a direct security
# review of this project's own WiFi flow). ".conf.d" drop-ins use
# "latest filename wins" for a given option (confirmed directly
# against systemd's own real documentation, man/standard-conf.xml -
# the opposite convention from preset.d files, where the EARLIEST
# name wins instead) - this file's own "10-" prefix sorts after
# systemd-conf's own "00-systemd-conf.conf", so this correctly
# overrides it, and "10-" is itself within systemd's own documented
# recommended range (10-40) for vendor/package-supplied drop-ins
# specifically (as opposed to the 60-90 range reserved for local
# administrator overrides under /etc/).
SRC_URI += "file://10-storage-volatile.conf"

do_install:append() {
    install -Dm 0644 ${UNPACKDIR}/10-storage-volatile.conf ${D}${systemd_unitdir}/journald.conf.d/10-storage-volatile.conf
}

FILES:${PN} += "${systemd_unitdir}/journald.conf.d/10-storage-volatile.conf"
