#!/usr/bin/env bash
# Factory-provision the A/B bundle root with the v1.0.0 UI in slot A.
#   tools/provision_factory.sh <bundle-root> [version]
# Idempotent: does nothing if <bundle-root>/current already exists.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BR="${1:?bundle-root}"
VERSION="${2:-1.0.0}"
mkdir -p "$BR"
if [[ -L "$BR/current" ]]; then
  echo "bundle root already provisioned -> $(readlink "$BR/current")"; exit 0
fi
FACTORY="${FACTORY_BUNDLE_DIR:-}"
if [[ -n "$FACTORY" && -d "$FACTORY" ]]; then
  rm -rf "$BR/slotA" && cp -r "$FACTORY" "$BR/slotA"
else
  "$ROOT/tools/build_bundle.sh" "$VERSION" --dir "$BR/slotA" >/dev/null
fi
mkdir -p "$BR/slotB"
ln -sfn slotA "$BR/current"
echo "provisioned $BR: slotA=$VERSION (active)"
