#!/usr/bin/env bash
# Build, hash and sign an HMI release and publish it to the OTA server root.
#
#   tools/publish_release.sh <version> <server-root> ["release notes"]
#
# Produces in <server-root>:
#   bundles/hmi-bundle-<version>.tar.gz
#   manifest.json      (what the vehicle should run)
#   manifest.json.sig  (base64 Ed25519 signature over the exact manifest bytes)
#
# Set SIGNING_KEY to override the key path (e.g. to simulate a rogue signer).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?version}"
SERVER_ROOT="${2:?server-root}"
NOTES="${3:-Maintenance release.}"
KEY="${SIGNING_KEY:-$ROOT/keys/ota_signing_key.pem}"

[[ -f "$KEY" ]] || "$ROOT/tools/gen_keys.sh"
mkdir -p "$SERVER_ROOT/bundles"

TAR="$("$ROOT/tools/build_bundle.sh" "$VERSION" "$SERVER_ROOT/bundles")"
SHA="$(sha256sum "$TAR" | cut -d' ' -f1)"
SIZE="$(stat -c %s "$TAR")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$VERSION" "$SHA" "$SIZE" "$NOTES" > "$TMP/manifest.json" <<'PY'
import json, sys, datetime
version, sha, size, notes = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
print(json.dumps({
    "schema": 1,
    "component": "cluster-hmi",
    "version": version,
    "minShellVersion": "1.0.0",
    "url": f"bundles/hmi-bundle-{version}.tar.gz",
    "sha256": sha,
    "size": size,
    "releaseNotes": notes,
    "publishedAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
}, indent=2))
PY

openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$TMP/manifest.json" -out "$TMP/manifest.sig"
base64 -w0 "$TMP/manifest.sig" > "$TMP/manifest.json.sig"

# Publish atomically: signature first, manifest last (agent fetches manifest then sig)
mv "$TMP/manifest.json.sig" "$SERVER_ROOT/manifest.json.sig.new"
mv "$TMP/manifest.json" "$SERVER_ROOT/manifest.json.new"
mv "$SERVER_ROOT/manifest.json.sig.new" "$SERVER_ROOT/manifest.json.sig"
mv "$SERVER_ROOT/manifest.json.new" "$SERVER_ROOT/manifest.json"

echo "published cluster-hmi $VERSION  sha256=$SHA  size=$SIZE"
