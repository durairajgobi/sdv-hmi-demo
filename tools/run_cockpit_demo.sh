#!/usr/bin/env bash
# Run the mixed-criticality cockpit natively: vehicle-sim + sdv-hypervisor
# (cluster partition opens a window, IVI partition renders into shared memory).
#   tools/run_cockpit_demo.sh
# Then, from another terminal, inject faults into the IVI and watch the cluster:
#   build/bin/sdv-hypervisor --status
#   build/bin/sdv-hypervisor --inject ivi:corrupt     (again to clear)
#   build/bin/sdv-hypervisor --inject ivi:hang
#   build/bin/sdv-hypervisor --inject ivi:cpu-storm   (again to clear)
#   build/bin/sdv-hypervisor --inject ivi:mem-bomb
#   build/bin/sdv-hypervisor --inject cluster:kill
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
DEMO="$ROOT/.demo"

if [[ ! -x "$BUILD/hmi/sdv-cluster-hmi" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" -j"$(nproc)"
fi
mkdir -p "$BUILD/bin"
for f in hmi/sdv-cluster-hmi cockpit/ivi-guest/sdv-ivi-guest cockpit/hypervisor/sdv-hypervisor vehicle-sim/vehicle-sim; do
  ln -sf "../$f" "$BUILD/bin/"
done
"$ROOT/tools/provision_factory.sh" "$DEMO/bundles" >/dev/null

export SDV_BIN_DIR="$BUILD/bin" HMI_BUNDLE_ROOT="$DEMO/bundles"
"$BUILD/bin/vehicle-sim" >/dev/null 2>&1 & SIM=$!
trap 'kill $SIM 2>/dev/null' EXIT
[[ "$(nproc)" -ge 2 ]] || echo "note: <2 CPUs, partitions will share a core"
exec "$BUILD/bin/sdv-hypervisor" --config "$ROOT/cockpit/partitions.conf"
