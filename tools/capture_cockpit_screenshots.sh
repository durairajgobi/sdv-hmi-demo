#!/usr/bin/env bash
# Capture README screenshots of the cockpit states into docs/screenshots.
# Uses a relaxed IVI watchdog so the "unavailable" state stays on screen long
# enough to photograph (the real config resets a hung IVI after 1 s).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
W="$(mktemp -d)"; OUT="$ROOT/docs/screenshots"; mkdir -p "$OUT" "$BUILD/bin"
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; true' EXIT
for f in hmi/sdv-cluster-hmi cockpit/ivi-guest/sdv-ivi-guest cockpit/hypervisor/sdv-hypervisor vehicle-sim/vehicle-sim; do
  ln -sf "../$f" "$BUILD/bin/"
done
sed 's/^watchdog_ms   = 1000/watchdog_ms   = 6000/' "$ROOT/cockpit/partitions.conf" > "$W/partitions.conf"

export SDV_BIN_DIR="$BUILD/bin" QT_QPA_PLATFORM=offscreen COCKPIT_SHM_DIR="$W/shm"
export HMI_BUNDLE_ROOT="$W/b" HMI_SNAP_DIR="$W/s" HMI_SNAP_INTERVAL=250 SD_PORT=35490 VEHICLE_SD_PORT=35490
"$ROOT/tools/provision_factory.sh" "$W/b" >/dev/null
HV="$BUILD/bin/sdv-hypervisor"
"$BUILD/bin/vehicle-sim" >/dev/null 2>&1 &
"$HV" --config "$W/partitions.conf" >/dev/null 2>&1 &

snap() { sleep "$1"; cp "$(ls -t "$W"/s/*.png | sed -n 2p)" "$OUT/$2.png"; echo "  $2.png"; }
snap 30 cockpit-1-ivi-projection
"$HV" --inject ivi:corrupt >/dev/null
snap 2 cockpit-2-degraded-native-fallback
"$HV" --inject ivi:corrupt >/dev/null
sleep 3; "$HV" --inject ivi:hang >/dev/null
snap 2.5 cockpit-3-ivi-unavailable
