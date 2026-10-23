#!/usr/bin/env bash
# Assemble an HMI UI bundle: common/ + (BASE chain) + <version>/ overlay.
#   tools/build_bundle.sh <version> <out-dir>   -> <out-dir>/hmi-bundle-<version>.tar.gz
#   tools/build_bundle.sh <version> --dir <dir> -> unpacked into <dir> (factory install)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/hmi/bundle-src"
VERSION="${1:?version}"
shift

[[ -d "$SRC/$VERSION" ]] || { echo "no bundle source for $VERSION" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/qml"
cp "$SRC/common/"*.qml "$STAGE/qml/"

# Resolve overlay chain (e.g. 1.2.0 -> 1.1.0) and apply oldest first
chain=("$VERSION")
v="$VERSION"
while [[ -f "$SRC/$v/BASE" ]]; do
  v="$(tr -d '[:space:]' < "$SRC/$v/BASE")"
  chain=("$v" "${chain[@]}")
done
for v in "${chain[@]}"; do
  find "$SRC/$v" -maxdepth 1 -name '*.qml' -exec cp {} "$STAGE/qml/" \;
done

cat > "$STAGE/bundle.json" <<EOF
{
  "component": "cluster-hmi",
  "version": "$VERSION",
  "entry": "qml/Main.qml",
  "builtAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

if [[ "${1:-}" == "--dir" ]]; then
  DEST="${2:?dir}"
  rm -rf "$DEST" && mkdir -p "$DEST"
  cp -r "$STAGE/." "$DEST/"
  echo "$DEST"
else
  OUT="${1:?out-dir}"
  mkdir -p "$OUT"
  TAR="$OUT/hmi-bundle-$VERSION.tar.gz"
  # Reproducible archive: fixed mtime/owner/order
  tar --sort=name --mtime='2026-01-01 00:00Z' --owner=0 --group=0 --numeric-owner \
      -C "$STAGE" -czf "$TAR" bundle.json qml
  echo "$TAR"
fi
