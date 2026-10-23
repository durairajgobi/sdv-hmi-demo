#!/usr/bin/env bash
# Container entrypoint for the publisher: publish.sh <version> ["release notes"]
set -euo pipefail
exec "$(dirname "$0")/publish_release.sh" "${1:?version}" "${OTA_SERVER_ROOT:-/srv/ota}" "${2:-Maintenance release.}"
