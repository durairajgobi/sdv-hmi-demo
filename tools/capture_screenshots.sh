#!/usr/bin/env bash
# Drive a scripted demo offscreen and save README screenshots to docs/screenshots.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
W="$(mktemp -d)"; OUT="$ROOT/docs/screenshots"
mkdir -p "$W/srv" "$OUT"
PIDS=(); trap 'kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$W"' EXIT

"$ROOT/tools/gen_keys.sh" >/dev/null
"$ROOT/tools/provision_factory.sh" "$W/b" >/dev/null
python3 "$ROOT/ota-server/server.py" --root "$W/srv" --port 18082 --rate 2500 >/dev/null 2>&1 & PIDS+=($!)
"$BUILD/vehicle-sim/vehicle-sim" >/dev/null 2>&1 & PIDS+=($!)
"$BUILD/ota-agent/sdv-ota-agent" --server http://127.0.0.1:18082/ --bundle-root "$W/b" \
  --public-key "$ROOT/keys/ota_signing_pub.pem" --work-dir "$W/dl" --poll-interval 1000 >/dev/null 2>&1 & PIDS+=($!)
QT_QPA_PLATFORM=offscreen HMI_AUTO_APPROVE=any HMI_AUTO_APPROVE_DELAY_MS=6000 \
  "$BUILD/hmi/sdv-cluster-hmi" --bundle-root "$W/b" --snap-dir "$W/s" --snap-interval 1000 >/dev/null 2>&1 & PIDS+=($!)

snap() { sleep "$1"; cp "$(ls -t "$W"/s/*.png | sed -n 2p)" "$OUT/$2.png"; echo "  $2.png"; }

snap 50 01-factory-1.0.0-driving
"$ROOT/tools/publish_release.sh" 1.1.0 "$W/srv" "New Horizon theme with live efficiency and eco-score widget." >/dev/null
snap 4 02-update-available
snap 4 03-downloading
snap 9 04-updated-1.1.0
snap 25 05-1.1.0-efficiency-widget
"$ROOT/tools/publish_release.sh" 1.2.0 "$W/srv" "Navigation preview." >/dev/null
snap 14 06-rolled-back
openssl genpkey -algorithm ed25519 -out "$W/rogue.pem" 2>/dev/null
SIGNING_KEY="$W/rogue.pem" "$ROOT/tools/publish_release.sh" 1.3.0 "$W/srv" "x" >/dev/null
snap 4 07-forged-release-rejected
