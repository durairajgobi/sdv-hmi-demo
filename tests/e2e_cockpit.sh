#!/usr/bin/env bash
# Fault-injection test for the hypervisor cockpit (freedom from interference).
#
# Boots vehicle-sim + sdv-hypervisor (cluster partition = Qt HMI, IVI partition
# = sdv-ivi-guest), then injects faults into the IVI and checks that the
# cluster detects/degrades/recovers WITHOUT restarting, and that the
# hypervisor contains or recovers the faulty partition:
#
#   1. boot             both partitions healthy, IVI navigation projected into cluster
#   2. corrupt frames   cluster rejects by CRC, degrades to native fallback, recovers
#   3. IVI hang         cluster hides stale guidance; watchdog resets the IVI only
#   4. ring flood       overload absorbed at the IVI side; cluster stays Healthy
#   5. memory leak      IVI dies at its RAM ceiling; cluster untouched
#   6. CPU storm        IVI threads confined to its core; no cluster watchdog trip
#   7. cluster crash    cluster restarts and re-attaches; IVI keeps running
#
# Usage: tests/e2e_cockpit.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
WORK="${E2E_WORK:-$(mktemp -d)}"
mkdir -p "$WORK" "$BUILD/bin"
for f in hmi/sdv-cluster-hmi cockpit/ivi-guest/sdv-ivi-guest cockpit/hypervisor/sdv-hypervisor vehicle-sim/vehicle-sim; do
  ln -sf "../$f" "$BUILD/bin/"
done
HV="$BUILD/bin/sdv-hypervisor"
LOG="$WORK/cockpit.log"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT

pass() { printf '  \e[32mPASS\e[0m %s\n' "$*"; }
die()  { printf '  \e[31mFAIL\e[0m %s\n' "$*"; echo "--- tail of $LOG"; tail -n 40 "$LOG"; exit 1; }
mark() { MARK=$(wc -l < "$LOG"); }
since() { tail -n +"$((MARK + 1))" "$LOG"; }
wait_for() {  # pattern timeout-s description   (only lines after the last mark)
  local deadline=$((SECONDS + $2))
  until since | grep -qE "$1"; do
    (( SECONDS < deadline )) || die "$3 (timeout waiting for /$1/)"
    sleep 0.2
  done
  pass "$3"
}
absent() {  # pattern description  (must not appear since mark)
  since | grep -qE "$1" && die "$2" || pass "$2"
}
pid_of() { "$HV" --status | awk -v p="$1" '$1==p {print $4}'; }

export SDV_BIN_DIR="$BUILD/bin" QT_QPA_PLATFORM=offscreen
export COCKPIT_SHM_DIR="$WORK/shm" HMI_BUNDLE_ROOT="$WORK/bundles"
export SD_PORT=34490 VEHICLE_SD_PORT=34490 HMI_SNAP_DIR="$WORK/snaps" HMI_SNAP_INTERVAL=1000
"$ROOT/tools/provision_factory.sh" "$HMI_BUNDLE_ROOT" >/dev/null
: > "$LOG"; MARK=0

"$BUILD/bin/vehicle-sim" >"$WORK/sim.log" 2>&1 & PIDS+=($!)
"$HV" --config "$ROOT/cockpit/partitions.conf" >>"$LOG" 2>&1 & PIDS+=($!)

echo "▶ 1. boot"
wait_for '\[cluster\] running' 15 "cluster partition up (GUI heartbeat + first frame)"
wait_for '\[ivi\] running' 15 "IVI partition up"
wait_for 'IVI projection .* -> Healthy' 10 "navigation projected from IVI into cluster"
CLUSTER=$(pid_of cluster)

echo "▶ 2. corrupt frames"
mark; "$HV" --inject ivi:corrupt >/dev/null
wait_for 'Healthy -> Degraded \(CRC mismatch' 3 "cluster rejected corrupt frames, native fallback shown"
mark; "$HV" --inject ivi:corrupt >/dev/null
wait_for 'Degraded -> Healthy' 3 "projection restored once frames are valid again"

echo "▶ 3. IVI hang"
mark; "$HV" --inject ivi:hang >/dev/null
wait_for 'IVI projection .* -> (Degraded|Unavailable)' 2 "cluster stopped trusting the stalled projection"
wait_for '\[ivi\] WATCHDOG: no heartbeat' 4 "hypervisor watchdog detected the hung IVI"
wait_for '\[ivi\] running' 8 "IVI partition reset and back"
wait_for 'IVI projection .* -> Healthy' 5 "navigation projection recovered"
absent '\[cluster\] (WATCHDOG|CRASHED)' "cluster partition never affected"

echo "▶ 4. inter-VM ring flood"
sleep 1; mark; "$HV" --inject ivi:flood >/dev/null
wait_for 'IVI ring overload' 6 "overload absorbed at the IVI side (bounded ring)"
sleep 2; "$HV" --inject ivi:flood >/dev/null
absent 'Healthy -> (Degraded|Unavailable)' "cluster stayed Healthy during the flood"

echo "▶ 5. IVI memory leak"
mark; "$HV" --inject ivi:mem-bomb >/dev/null
wait_for '\[ivi\] CRASHED \(signal Aborted\)' 15 "IVI hit its 1024 MB partition ceiling and died"
wait_for '\[ivi\] running' 8 "IVI restarted by the hypervisor"
[[ "$(pid_of cluster)" == "$CLUSTER" ]] && pass "cluster process unchanged (pid $CLUSTER)" || die "cluster restarted"

echo "▶ 6. IVI CPU storm"
mark; "$HV" --inject ivi:cpu-storm >/dev/null
sleep 8
"$HV" --inject ivi:cpu-storm >/dev/null
absent '\[cluster\] (WATCHDOG|CRASHED)' "cluster heartbeat on time throughout the storm"
since | grep 'sdv.kpi' | tail -1 | sed 's/^/        /'

echo "▶ 7. cluster crash"
IVI=$(pid_of ivi)
mark; "$HV" --inject cluster:kill >/dev/null
wait_for '\[cluster\] CRASHED' 3 "hypervisor saw the cluster die"
wait_for '\[cluster\] running' 10 "cluster partition restarted and rendering"
wait_for 'cluster generation 2' 5 "cluster re-attached to inter-VM memory (new generation)"
wait_for 'IVI projection .* -> Healthy' 5 "projection resumed without restarting the IVI"
[[ "$(pid_of ivi)" == "$IVI" ]] && pass "IVI process unchanged (pid $IVI)" || die "IVI restarted"

echo
"$HV" --status
echo
echo "All cockpit fault-injection scenarios passed. Screenshots: $WORK/snaps"
