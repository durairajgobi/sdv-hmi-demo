#!/usr/bin/env bash
# End-to-end OTA scenario test. Runs the whole stack natively (no containers):
#   ota-server, vehicle-sim, ota-agent, cluster HMI (offscreen, auto-approve)
#
#   1. factory 1.0.0 boots and is confirmed healthy
#   2. signed 1.1.0 is offered, installed to slot B, hot-swapped, confirmed   -> Success
#   3. 1.2.0 (incompatible QML) fails to load, HMI keeps 1.1.0 on screen      -> RolledBack
#   4. 1.3.0 signed with a rogue key                                         -> Failed (rejected)
#   5. 1.3.0 correctly signed but bundle tampered in transit                 -> Failed (sha256)
#
# Usage: tests/e2e_ota.sh [build-dir]     (screenshots land in $WORK/snaps)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
WORK="${E2E_WORK:-$(mktemp -d)}"
SRV="$WORK/server"; BR="$WORK/bundles"; LOG="$WORK/logs"
mkdir -p "$SRV" "$LOG"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT

pass() { printf '  \e[32mPASS\e[0m %s\n' "$*"; }
die()  { printf '  \e[31mFAIL\e[0m %s\n' "$*"; echo "--- logs in $LOG"; tail -n 25 "$LOG"/*.log; exit 1; }
wait_for() {  # file pattern timeout-s description
  local deadline=$((SECONDS + $3))
  until grep -qE "$2" "$1" 2>/dev/null; do
    (( SECONDS < deadline )) || die "$4 (timeout waiting for /$2/ in $(basename "$1"))"
    sleep 0.3
  done
  pass "$4"
}

# Use dedicated ports so the test never collides with a running demo
export SD_PORT=31490
VEH_SD=31490; HTTP_PORT=18080

"$ROOT/tools/gen_keys.sh" >/dev/null
"$ROOT/tools/provision_factory.sh" "$BR" >/dev/null

python3 "$ROOT/ota-server/server.py" --root "$SRV" --port $HTTP_PORT --rate 8000 >"$LOG/server.log" 2>&1 & PIDS+=($!)
"$BUILD/vehicle-sim/vehicle-sim" >"$LOG/sim.log" 2>&1 & PIDS+=($!)
"$BUILD/ota-agent/sdv-ota-agent" --server "http://127.0.0.1:$HTTP_PORT/" --bundle-root "$BR" \
    --public-key "$ROOT/keys/ota_signing_pub.pem" --work-dir "$WORK/dl" \
    --poll-interval 1500 --health-timeout 15000 >"$LOG/agent.log" 2>&1 & PIDS+=($!)
QT_QPA_PLATFORM=offscreen HMI_AUTO_APPROVE=any \
"$BUILD/hmi/sdv-cluster-hmi" --bundle-root "$BR" --vehicle-sd-port $VEH_SD \
    --snap-dir "$WORK/snaps" --snap-interval 1500 --health-window 2000 >"$LOG/hmi.log" 2>&1 & PIDS+=($!)

echo "▶ 1. factory boot"
wait_for "$LOG/hmi.log" 'VehicleSignals.*subscription acknowledged' 10 "HMI subscribed to VehicleSignals over SOME/IP-SD"
wait_for "$LOG/hmi.log" 'OtaUpdate.*subscription acknowledged' 10 "HMI subscribed to OtaUpdate"
wait_for "$LOG/hmi.log" 'bundle "1.0.0" stable' 10 "factory UI 1.0.0 rendered and stable"

echo "▶ 2. signed update 1.1.0"
"$ROOT/tools/publish_release.sh" 1.1.0 "$SRV" "New Horizon theme and live efficiency widget." >/dev/null
wait_for "$LOG/agent.log" 'UpdateAvailable' 10 "agent verified signature and offered 1.1.0"
wait_for "$LOG/agent.log" 'Downloading' 10 "driver consent -> download started"
wait_for "$LOG/agent.log" 'Activating' 30 "sha256 verified, installed to slot B, activated"
wait_for "$LOG/hmi.log" 'loading UI bundle 1.1.0 from slot B \(hot swap\)' 10 "HMI hot-swapped to 1.1.0 without restart"
wait_for "$LOG/agent.log" 'committed' 15 "HMI health confirmed -> update committed"
grep -q '"committedSlot": "B"' "$BR/ota-state.json" && pass "persistent state: committed slot B" || die "state not committed"

echo "▶ 3. broken update 1.2.0 -> automatic rollback"
"$ROOT/tools/publish_release.sh" 1.2.0 "$SRV" "Navigation preview." >/dev/null
wait_for "$LOG/hmi.log" 'bundle 1.2.0 failed to load' 40 "HMI detected 1.2.0 cannot load"
wait_for "$LOG/hmi.log" 'keeping previous UI 1.1.0' 5 "previous UI kept on screen (no blank cluster)"
wait_for "$LOG/agent.log" 'ROLLBACK of 1.2.0 -> 1.1.0' 10 "agent rolled back to slot B / 1.1.0"
[[ "$(readlink "$BR/current")" == "slotB" ]] && pass "current -> slotB" || die "symlink not restored"
grep -q '"1.2.0"' "$BR/ota-state.json" && pass "1.2.0 blacklisted (not re-offered)" || die "1.2.0 not rejected"

echo "▶ 4. manifest signed by rogue key"
openssl genpkey -algorithm ed25519 -out "$WORK/rogue.pem" 2>/dev/null
SIGNING_KEY="$WORK/rogue.pem" "$ROOT/tools/publish_release.sh" 1.3.0 "$SRV" "Totally legit." >/dev/null
wait_for "$LOG/agent.log" 'Release manifest rejected: signature does not match' 10 "forged manifest rejected"
grep -q 'download-1.3.0' "$LOG/agent.log" && die "agent downloaded from forged manifest" || pass "nothing downloaded"

echo "▶ 5. bundle tampered in transit"
STAGE="$WORK/stage"; mkdir -p "$STAGE"
"$ROOT/tools/publish_release.sh" 1.3.0 "$STAGE" "Bugfixes." >/dev/null
printf 'X' | dd of="$STAGE/bundles/hmi-bundle-1.3.0.tar.gz" bs=1 seek=200 conv=notrunc 2>/dev/null
cp "$STAGE/bundles/hmi-bundle-1.3.0.tar.gz" "$SRV/bundles/"
cp "$STAGE/manifest.json.sig" "$SRV/"; cp "$STAGE/manifest.json" "$SRV/"
wait_for "$LOG/agent.log" 'Integrity check failed' 30 "tampered bundle rejected by sha256"
[[ "$(readlink "$BR/current")" == "slotB" ]] && pass "vehicle still on 1.1.0 / slot B" || die "slot changed"

echo
echo "All E2E scenarios passed. Screenshots: $WORK/snaps"
