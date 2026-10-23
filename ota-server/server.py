#!/usr/bin/env python3
"""Minimal OTA backend: serves signed release manifests and bundles.

Static file server (stdlib only) with:
  * no-store caching so vehicles always see the latest manifest
  * optional bandwidth throttling to emulate a cellular link (--rate)
  * a tiny /healthz endpoint for container orchestration

Releases are produced by tools/publish_release.sh, which signs the manifest
with the backend's Ed25519 key. The server itself holds no secrets.
"""
import argparse
import functools
import http.server
import os
import sys
import time


class OtaHandler(http.server.SimpleHTTPRequestHandler):
    rate_bps = 0  # 0 = unthrottled

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if self.path == "/healthz":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def copyfile(self, source, outputfile):
        if not self.rate_bps or not self.path.startswith("/bundles/"):
            return super().copyfile(source, outputfile)
        chunk = max(512, self.rate_bps // 10)
        while True:
            buf = source.read(chunk)
            if not buf:
                break
            outputfile.write(buf)
            outputfile.flush()
            time.sleep(len(buf) / self.rate_bps)

    def log_message(self, fmt, *args):
        sys.stdout.write("[ota-server] %s %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.environ.get("OTA_SERVER_ROOT", "/srv/ota"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("OTA_SERVER_PORT", "8080")))
    ap.add_argument("--rate", type=int, default=int(os.environ.get("OTA_RATE_BPS", "6000")),
                    help="bundle download rate in bytes/s (0 = unlimited)")
    args = ap.parse_args()

    os.makedirs(args.root, exist_ok=True)
    OtaHandler.rate_bps = args.rate
    handler = functools.partial(OtaHandler, directory=args.root)
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler)
    print(f"[ota-server] serving {args.root} on :{args.port} (rate {args.rate or 'unlimited'} B/s)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
