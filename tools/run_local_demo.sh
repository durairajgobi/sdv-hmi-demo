#!/usr/bin/env bash
# Run the full SDV stack natively on a Linux desktop (no containers).
#   tools/run_local_demo.sh            # opens the cluster window
# Then, in a second terminal:
#   tools/publish_release.sh 1.1.0 .demo/server "New Horizon theme + efficiency widget"
#   tools/publish_release.sh 1.2.0 .demo/server "Navigation preview"   # -> rollback
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
DEMO="$ROOT/.demo"

if [[ ! -x "$BUILD/hmi/sdv-cluster-hmi" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" -j"$(nproc)"
fi

"$ROOT/tools/gen_keys.sh" >/dev/null
"$ROOT/tools/provision_factory.sh" "$DEMO/bundles"
mkdir -p "$DEMO/server" "$DEMO/logs"

PIDS=()
trap 'kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null' EXIT INT TERM

python3 "$ROOT/ota-server/server.py" --root "$DEMO/server" --port 8080 >"$DEMO/logs/server.log" 2>&1 & PIDS+=($!)
"$BUILD/vehicle-sim/vehicle-sim" >"$DEMO/logs/sim.log" 2>&1 & PIDS+=($!)
"$BUILD/ota-agent/sdv-ota-agent" --server http://127.0.0.1:8080/ --bundle-root "$DEMO/bundles" \
  --public-key "$ROOT/keys/ota_signing_pub.pem" --work-dir "$DEMO/dl" --poll-interval 3000 \
  >"$DEMO/logs/agent.log" 2>&1 & PIDS+=($!)

echo "Stack up. Logs: $DEMO/logs   (tail -f $DEMO/logs/agent.log)"
echo "Publish an update from another terminal:"
echo "  tools/publish_release.sh 1.1.0 .demo/server \"New Horizon theme and live efficiency widget\""
"$BUILD/hmi/sdv-cluster-hmi" --bundle-root "$DEMO/bundles" 2>&1 | tee "$DEMO/logs/hmi.log"
