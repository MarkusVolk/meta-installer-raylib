# OPT-IN, DEVELOPER-ONLY: bakes a self-signed test certificate into
# this image's CA trust bundle at build time, so the app's curl call
# (backend.hpp, deliberately never passes -k/--insecure - see that
# file's own comments) accepts HTTPS downloads from a LOCAL testing
# server run via scripts/serve-https.py without the person needing to
# trust the cert on the machine running the app at runtime, which -
# for this project's target images - would need to happen every boot
# on a RAM-resident/live system and doesn't persist anyway.
#
# NEVER active by default and safe to keep in the layer for any real
# deployment build: entirely gated on whether a specific file
# (files/serve-https-test.crt) has actually been placed here by a
# developer - checked at parse time via an inline "${@...}" expression
# below (not a separate python() block for the check itself - THISDIR
# inside a bare python() block was found to resolve to the ORIGINAL
# recipe's own directory rather than this bbappend's, verified
# directly; the inline expression shares FILESEXTRAPATHS's own,
# correctly-resolving evaluation context instead).
#
# HOW TO USE:
#   1. Find the LAN IP the installer will actually reach this server
#      at (same method serve-https.py itself uses to print its own
#      URL):
#        python3 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.connect(('8.8.8.8',80)); print(s.getsockname()[0]); s.close()"
#   2. Generate a STABLE cert+key ONCE using that IP (serve-https.py's
#      own default behaviour re-generates a NEW, different self-signed
#      cert into a fresh /tmp/serve-https-cert-XXXXXX/ directory on
#      every run, which wouldn't stay valid across separate builds/
#      test sessions - reuse the SAME one instead). The
#      subjectAltName (SAN) is required, not optional - curl (and
#      modern TLS clients generally) reject a CN-only certificate as
#      a hostname mismatch even once the CA itself is trusted; must
#      match the actual IP from step 1, not a placeholder:
#        openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
#            -keyout serve-https-test.key -out serve-https-test.crt \
#            -subj "/CN=<your-ip-from-step-1>" \
#            -addext "subjectAltName=DNS:<your-ip-from-step-1>,IP:<your-ip-from-step-1>,IP:127.0.0.1"
#   3. Copy serve-https-test.crt (only the certificate - NOT the
#      private key) into this directory:
#        cp serve-https-test.crt <this layer>/recipes-core/ca-certificates/files/
#   4. Rebuild this image (bitbake core-image-installer-raylib /
#      core-image-installer-raylib-initramfs as appropriate) - the
#      cert is now trusted by curl in the built image.
#   5. Run scripts/serve-https.py with --cert/--key pointing at the
#      SAME cert+key from step 2, so it keeps presenting the exact
#      certificate the image now trusts:
#        serve-https.py --cert serve-https-test.crt --key serve-https-test.key
#
# Remove files/serve-https-test.crt again (and rebuild) once done
# testing, so a real deployment build doesn't carry a test-only,
# locally-generated trust anchor around indefinitely.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

INSTALL_SERVE_HTTPS_TEST_CERT := "${@'1' if os.path.exists(d.expand('${THISDIR}/files/serve-https-test.crt')) else '0'}"

python() {
    if d.getVar('INSTALL_SERVE_HTTPS_TEST_CERT') == '1':
        bb.warn("ca-certificates: baking in files/serve-https-test.crt (developer opt-in, "
                "serve-https.py testing only) - remove it before a real deployment build.")
        d.appendVar('SRC_URI', ' file://serve-https-test.crt')
}

do_install:append() {
    if [ "${INSTALL_SERVE_HTTPS_TEST_CERT}" = "1" ]; then
        install -m 0644 ${UNPACKDIR}/serve-https-test.crt ${D}${datadir}/ca-certificates/serve-https-test.crt
        echo "serve-https-test.crt" >> ${D}${sysconfdir}/ca-certificates.conf
    fi
}
