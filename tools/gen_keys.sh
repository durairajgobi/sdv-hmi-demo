#!/usr/bin/env bash
# Generate the Ed25519 OTA signing key pair.
#   private key -> keys/ota_signing_key.pem  (backend only, never on the vehicle)
#   public key  -> keys/ota_signing_pub.pem  (baked into the vehicle image)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/keys"
if [[ -f "$ROOT/keys/ota_signing_key.pem" && "${1:-}" != "--force" ]]; then
  echo "keys already exist (use --force to regenerate)"; exit 0
fi
openssl genpkey -algorithm ed25519 -out "$ROOT/keys/ota_signing_key.pem"
openssl pkey -in "$ROOT/keys/ota_signing_key.pem" -pubout -out "$ROOT/keys/ota_signing_pub.pem"
chmod 600 "$ROOT/keys/ota_signing_key.pem"
echo "generated keys/ota_signing_{key,pub}.pem"
