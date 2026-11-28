#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
CONTROLLER_BIN="${CONTROLLER_BIN:-$REPO_ROOT/build-controller/controller/circuit_controller}"

: "${BOARD2_IP:?Set BOARD2_IP to the current Board 2 IPv4 address}"
: "${BOARD3_IP:?Set BOARD3_IP to the current Board 3 IPv4 address}"

if [[ ! -x "$CONTROLLER_BIN" ]]; then
    printf 'Controller executable is missing or not executable: %s\n' "$CONTROLLER_BIN" >&2
    printf 'Build with: cmake --build build-controller --target circuit_controller\n' >&2
    exit 2
fi

exec "$CONTROLLER_BIN" \
    --bind-ip "${CONTROLLER_BIND_IP:-0.0.0.0}" \
    --board2-ip "$BOARD2_IP" \
    --board3-ip "$BOARD3_IP" \
    --command-timeout-ms "${COMMAND_TIMEOUT_MS:-500}" \
    --measurement-timeout-ms "${MEASUREMENT_TIMEOUT_MS:-15000}" \
    --cleanup-timeout-ms "${CLEANUP_TIMEOUT_MS:-1000}" \
    --retries "${RETRIES:-2}"
