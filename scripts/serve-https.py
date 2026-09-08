#!/usr/bin/env python3
"""Serve the current (or given) directory over HTTPS - for locally
testing the installer app's "HTTPS URL" source option
(rootfs.tar.gz / .wic downloads) without needing a real server.

Also supports plain HTTP via --http, for testing the app's separate
"HTTP" source tab instead - deliberately no TLS at all there (that
tab exists specifically to avoid certificate hassle on a trusted home
network), so none of the cert-trust concerns below apply in that mode.

Stdlib only (http.server + ssl), no dependencies. Auto-generates a
self-signed certificate on first run via the "openssl" CLI tool if
none is given (HTTPS mode only - --http needs no certificate at all).

IMPORTANT (HTTPS mode only): the app's curl call (backend.hpp) does
NOT pass "-k"/"--insecure" - by design, so the shipped tool never
silently skips certificate verification. A self-signed cert from this
script will therefore be REJECTED by the app as-is. To actually test
against it, either:
  (a) trust this cert on the test machine (see printed instructions),
  (b) temporarily add "-k" to the curl call in backend.hpp for a local
      test build only (revert before using the real image), or
  (c) point the app at a URL with a real, properly signed certificate
      instead of this local server.

Usage:
    serve-https.py [-d DIR] [-p PORT] [--cert CERT --key KEY]
    serve-https.py --http [-d DIR] [-p PORT]
"""

import argparse
import http.server
import os
import socket
import ssl
import subprocess
import sys
import tempfile


def generate_self_signed_cert(cert_path: str, key_path: str, hostname: str) -> None:
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key_path, "-out", cert_path,
            "-days", "365", "-nodes",
            "-subj", f"/CN={hostname}",
            "-addext", f"subjectAltName=DNS:{hostname},IP:127.0.0.1",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def local_ip() -> str:
    # No actual traffic sent - just used to let the OS pick the route/
    # interface it would use, so we can print a LAN-reachable URL
    # instead of only "localhost".
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-d", "--dir", default=".", help="directory to serve (default: current directory)")
    parser.add_argument("-p", "--port", type=int, default=None, help="port (default: 8443 for HTTPS, 8000 for --http)")
    parser.add_argument("--http", action="store_true", help="plain HTTP, no TLS at all - for testing the app's separate HTTP tab")
    parser.add_argument("--cert", help="existing certificate (PEM) - skips self-signed generation (HTTPS mode only)")
    parser.add_argument("--key", help="matching private key (PEM), required with --cert (HTTPS mode only)")
    args = parser.parse_args()

    if args.http and (args.cert or args.key):
        parser.error("--cert/--key make no sense with --http (no TLS at all in that mode)")
    if args.port is None:
        args.port = 8000 if args.http else 8443

    if bool(args.cert) != bool(args.key):
        parser.error("--cert and --key must be given together")

    serve_dir = os.path.abspath(args.dir)
    if not os.path.isdir(serve_dir):
        print(f"E: not a directory: {serve_dir}", file=sys.stderr)
        return 1

    ip = local_ip()

    tmp_dir = None
    cert_path = key_path = None
    if not args.http:
        if args.cert:
            cert_path, key_path = args.cert, args.key
        else:
            tmp_dir = tempfile.mkdtemp(prefix="serve-https-cert-")
            cert_path = os.path.join(tmp_dir, "cert.pem")
            key_path = os.path.join(tmp_dir, "key.pem")
            try:
                generate_self_signed_cert(cert_path, key_path, ip)
            except (subprocess.CalledProcessError, FileNotFoundError) as e:
                print(f"E: could not generate a self-signed certificate via openssl: {e}", file=sys.stderr)
                return 1
            print(f"Self-signed certificate generated (valid for {ip}, 365 days): {cert_path}")

    handler_cls = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(*a, directory=serve_dir, **kw)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler_cls)

    if not args.http:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert_path, keyfile=key_path)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    scheme = "http" if args.http else "https"
    print(f"Serving {serve_dir}")
    print(f"  {scheme}://{ip}:{args.port}/")
    print(f"  {scheme}://localhost:{args.port}/  (only from this machine)")
    if args.http:
        print()
        print("Plain HTTP, no certificate involved at all - for the app's separate")
        print("\"HTTP\" source tab specifically (not \"HTTPS URL\").")
    elif tmp_dir:
        print()
        print("Self-signed certificate - the app's curl call rejects this by")
        print("default (no -k/--insecure, by design). To test against it:")
        print(f"  - trust it: sudo cp {cert_path} /usr/local/share/ca-certificates/serve-https-test.crt")
        print("             sudo update-ca-certificates   # target may use a different path/tool")
        print("  - or temporarily add -k to the curl call in backend.hpp for a local test build")
    print()
    print("Ctrl+C to stop.")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        if tmp_dir:
            try:
                os.unlink(cert_path)
                os.unlink(key_path)
                os.rmdir(tmp_dir)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
