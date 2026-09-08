# shared-mime-info (and therefore glib-2.0, its own dependency) was
# an unavoidable-seeming build-time cost of using systemd as init -
# see the README's own earlier entry on this ("shared-mime-info/
# glib-2.0: echte, unvermeidbare Bauzeit-Abhaengigkeit von systemd").
# Revisited after a direct question about whether removing it here
# would actually work.
#
# Checked systemd's own recipe and oe-core's mime.bbclass directly
# (real, current GitHub source, not assumed): systemd ships its own
# static mime-type files into ${MIMEDIR}, packaged into a SEPARATE,
# merely RRECOMMENDS'd "${PN}-mime" sub-package (systemd_*.bb: "FILES:
# ${PN}-mime = "${MIMEDIR}"", "RRECOMMENDS:${PN} += "${PN}-mime"") -
# which this image never installs anyway: "systemd-mime" is listed in
# BAD_RECOMMENDATIONS (see conf/distro/installer-minimal-raylib.conf -
# an earlier version of this comment claimed NO_RECOMMENDATIONS=1 was
# set, which was never actually the case; found via a README audit). mime.bbclass's shared-mime-info DEPENDS is added purely at
# the class level, unconditionally, regardless of whether the "-mime"
# subpackage is ever actually consumed - nothing in systemd's own
# meson build/configure step needs shared-mime-info's tools present to
# compile (the class's actual use of the dependency - update-mime-
# database postinst/postrm scripts - only ever gets attached to
# whichever package(s) end up containing mime-type XML files, i.e.
# just "${PN}-mime" here, per mime.bbclass's own populate_packages
# logic - checked directly, not assumed).
#
# Verified for real, not just reasoned about: built an isolated
# bitbake+oe-core test environment with this exact bbappend, confirmed
# shared-mime-info disappears from systemd's own DEPENDS (bitbake -e),
# and re-resolved the full core-image-installer-raylib dependency
# graph (bitbake -g) - neither "shared-mime-info" nor "glib-2.0" (its
# own dependency) appear anywhere in the resulting build list at all,
# confirming nothing else in this image needs glib-2.0 either.
DEPENDS:remove = "shared-mime-info"

# Found via a real build failure (do_package_qa QA check, "build-deps"):
# removing the recipe-wide DEPENDS above wasn't enough on its own -
# mime.bbclass's populate_packages logic still attaches RDEPENDS on
# shared-mime-info-data to the "${PN}-mime" sub-package specifically
# (the one containing the actual MIMEDIR files, see the earlier
# comment on FILES:${PN}-mime), regardless of the recipe-level DEPENDS
# change. oe-core's QA correctly flags this as inconsistent: a package
# RDEPENDS on something the recipe no longer has a build-time DEPENDS
# for. Since this sub-package is only ever RRECOMMENDS'd (never
# RDEPENDS'd) and this image never installs it anyway
# (BAD_RECOMMENDATIONS), its own RDEPENDS on shared-mime-info-data is
# equally moot - removed here too, resolving the inconsistency at its
# actual source rather than suppressing the QA warning about it.
RDEPENDS:systemd-mime:remove = "shared-mime-info-data"

# PACKAGECONFIG cleanup: disabling systemd features this rescue/
# installer image genuinely doesn't use. Only the CLEAR candidates
# from a full review of systemd's own PACKAGECONFIG list (checked
# against the real, current oe-core source) - kept several others
# deliberately enabled where this project has real, hard-won
# dependencies on them (serial-getty-generator: our own debug-console
# workflow; networkd: our only network bring-up mechanism, needed for
# curl/HTTPS downloads; vconsole: keyboard layout/font on that same
# debug console; set-time-epoch: cheap protection against TLS
# certificate validation failures with no RTC) or where removing them
# seemed riskier than the space saved (logind: unresolved VT-switch
# behaviour investigated earlier in this project; sysusers: other
# packages may implicitly rely on it; resolved/timesyncd/nss/nss-
# resolve/kmod/randomseed/zstd: genuinely ambiguous, left for a
# separate, more careful pass rather than decided here).
#
#   backlight        - display backlight control, no such UI here
#   binfmt            - foreign-arch binary format registration, unused
#   coredump          - crash dump handling, a debugging aid rather
#                        than something this image's own operation
#                        needs (own crash investigations in this
#                        project so far were all done via journalctl/
#                        real hardware testing, not systemd-coredump)
#   gshadow           - group shadow passwords, no fine-grained group
#                        password management here
#   hibernate         - suspend-to-disk, no power management UI here
#   hostnamed         - D-Bus hostname management service, no UI to
#                        change it
#   ima               - Integrity Measurement Architecture (security
#                        attestation), not relevant for a rescue image
#   localed           - D-Bus locale management service, no UI to
#                        change it
#   machined          - VM/container management integration, this
#                        image does neither
#   myhostname        - NSS module resolving own hostname to loopback,
#                        not needed
#   nss-mymachines    - NSS for machined/container name resolution,
#                        consistent with removing machined above
#   osc-context       - shell integration for interactive terminal
#                        emulators (OSC escape sequences, e.g.
#                        clickable directory links) - purely cosmetic
#                        for interactive shells, irrelevant to this
#                        GUI-driven app (verified directly what this
#                        flag does: installs /etc/profile.d/80-
#                        systemd-osc-context.sh, nothing else)
#   quotacheck        - disk quota checking, no user quota management
#                        here
#   timedated         - D-Bus time/timezone management service, no UI
#                        to change it
#   userdb            - systemd's userdb multiplexer (dynamic-user/
#                        Home infrastructure), not used here
#   utmp              - login records (who/last), not meaningful for
#                        a single-purpose rescue installer
#   wheel-group       - "wheel" group for sudo-like privilege
#                        separation, this image runs everything as
#                        root directly
PACKAGECONFIG:remove = "\
    backlight \
    binfmt \
    coredump \
    gshadow \
    hibernate \
    hostnamed \
    ima \
    localed \
    machined \
    myhostname \
    nss-mymachines \
    osc-context \
    quotacheck \
    timedated \
    userdb \
    utmp \
    wheel-group \
    "

# systemd-resolved.service enablement: this is bundled directly into
# the main "${PN}" package itself when the "resolved" PACKAGECONFIG
# is active (checked directly against the real recipe - no separate
# "systemd-resolved" package exists at all, an incorrect first
# assumption caught via a real build error, "Nothing RPROVIDES
# 'systemd-resolved'", before settling on this approach), so
# SYSTEMD_AUTO_ENABLE (this project's own usual mechanism, used
# throughout its other recipes) doesn't apply here - that's for
# recipe-defined services, not ones bundled inside a package this
# project doesn't otherwise control the packaging of.
#
# oe-core's own default preset policy (99-default.preset: "disable *")
# disables every systemd service, including this one, unless an
# earlier (lower-numbered, higher-priority) preset file says
# otherwise - confirmed directly against systemd's own real
# documentation (systemd.preset.xml: "All preset files are sorted by
# their filename in lexicographic order... the entry in the file with
# the lexicographically earliest name will be applied"). This file's
# own "10-" prefix sorts before oe-core's own "99-", so this wins.
#
# Root cause this addresses: iwd only ever handles the WiFi LINK layer
# (WPA handshake) - see installer-minimal-raylib.conf's own extensive
# comment on iwd's own EnableNetworkConfiguration/NameResolvingService
# options (found via a real user report - WiFi looked connected via
# iwctl, but plain curl still failed outright) - iwd's own main.conf
# (recipes-connectivity/iwd's own bbappend) pushes DHCP-provided DNS
# servers to systemd-resolved specifically, which is useless if this
# service was never actually running in the first place.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://10-enable-resolved.preset"

do_install:append() {
    install -Dm 0644 ${UNPACKDIR}/10-enable-resolved.preset ${D}${systemd_unitdir}/system-preset/10-enable-resolved.preset
}

FILES:${PN} += "${systemd_unitdir}/system-preset/10-enable-resolved.preset"
