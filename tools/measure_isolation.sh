#!/usr/bin/env bash
# Measure freedom-from-interference: cluster UI scheduling latency and frame
# timing while the IVI partition runs a CPU storm, with partitioning ON
# (CPU pinning + priorities + memory ceiling) and OFF (shared CPUs).
#
#   tools/measure_isolation.sh [build-dir]
# Prints a markdown table. Numbers depend on the host; run on an idle machine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
W="${KEEP:-$(mktemp -d)}"; mkdir -p "$W"
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; [[ -n "${KEEP:-}" ]] || rm -rf "$W"; true' EXIT

mkdir -p "$BUILD/bin"
for f in hmi/sdv-cluster-hmi cockpit/ivi-guest/sdv-ivi-guest cockpit/hypervisor/sdv-hypervisor vehicle-sim/vehicle-sim; do
  ln -sf "../$f" "$BUILD/bin/"
done
export SDV_BIN_DIR="$BUILD/bin" QT_QPA_PLATFORM=offscreen
export SD_PORT=33490 VEHICLE_SD_PORT=33490

run() {  # $1 = label, $2 = extra hypervisor flag
  local dir="$W/$1"; mkdir -p "$dir"
  "$ROOT/tools/provision_factory.sh" "$dir/b" >/dev/null
  export HMI_BUNDLE_ROOT="$dir/b" COCKPIT_SHM_DIR="$dir/shm"
  "$BUILD/bin/vehicle-sim" >/dev/null 2>&1 & local sim=$!
  "$BUILD/bin/sdv-hypervisor" --config "$ROOT/cockpit/partitions.conf" $2 >>"$dir/hv.log" 2>&1 & local hv=$!
  sleep 11;  echo "MARK baseline" >>"$dir/hv.log"
  sleep 15;  "$BUILD/bin/sdv-hypervisor" --inject ivi:cpu-storm >/dev/null
  echo "MARK storm" >>"$dir/hv.log"
  sleep 16;  "$BUILD/bin/sdv-hypervisor" --inject ivi:cpu-storm >/dev/null
  kill "$hv" "$sim"; wait "$hv" "$sim" 2>/dev/null || true
}

echo "running with partitioning ON ..." >&2;  run isolated ""
echo "running with partitioning OFF ..." >&2; run shared "--no-isolation"

python3 - "$W" <<'PY'
import re, sys, statistics, os
root = sys.argv[1]
rx = re.compile(r"frame_p99=([\d.]+)ms frame_max=([\d.]+)ms jank\(>50ms\)=(\d+) \| ui_latency p50=([\d.]+)ms p99=([\d.]+)ms max=([\d.]+)ms")
print("| Partitioning | Phase | UI-thread latency p99 | UI-thread latency max | Frame interval p99 | Janky frames (>50 ms) |")
print("|---|---|---|---|---|---|")
for label, name in (("isolated", "ON"), ("shared", "OFF")):
    phase, rows = None, {"baseline": [], "storm": []}
    for line in open(os.path.join(root, label, "hv.log"), errors="replace"):
        if line.startswith("MARK"): phase = line.split()[1]; continue
        m = rx.search(line)
        if m and phase: rows[phase].append([float(x) for x in m.groups()])
    for ph in ("baseline", "storm"):
        r = rows[ph][1:] or rows[ph]  # skip the window that straddles the switch
        if not r: continue
        med = lambda i: statistics.median(x[i] for x in r)
        mx = lambda i: max(x[i] for x in r)
        print(f"| {name} | {'idle IVI' if ph=='baseline' else 'IVI CPU storm'} | {med(4):.1f} ms | {mx(5):.1f} ms | {med(0):.1f} ms | {int(sum(x[2] for x in r))} |")
PY
