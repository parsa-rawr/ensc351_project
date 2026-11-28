#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
CROSS_COMPILER="${CROSS_COMPILER:-aarch64-linux-gnu-gcc}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-arm64-controller}"
JOBS="${JOBS:-4}"

usage() {
    cat <<'EOF'
Usage: build_deploy_arm64.sh [--build-only]

Cross-build circuit_controller as a statically linked ARM64 Linux executable.
Without --build-only, verify SSH reachability first and SCP the executable.

Required for transfer:
  TARGET_SSH_USER       SSH username on Board 1
  TARGET_SSH_ADDRESS    Board 1 IPv4 address or SSH hostname

Optional:
  REMOTE_BINARY_PATH    destination path (default: /home/USER/circuit_controller)
  CROSS_COMPILER        cross compiler (default: aarch64-linux-gnu-gcc)
  BUILD_DIR             CMake build directory
  JOBS                  parallel build jobs (default: 4)

Use --build-only to build and verify the binary without any network access.
EOF
}

BUILD_ONLY=false
case "${1:-}" in
    "") ;;
    --build-only) BUILD_ONLY=true ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

if ! command -v "$CROSS_COMPILER" >/dev/null 2>&1; then
    printf 'Missing cross compiler: %s\n' "$CROSS_COMPILER" >&2
    printf 'Install the aarch64-linux-gnu GCC cross toolchain and static libc development files.\n' >&2
    exit 2
fi
command -v cmake >/dev/null
command -v file >/dev/null

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DENABLE_HARDWARE=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CROSS_COMPILER" \
    -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build "$BUILD_DIR" --target circuit_controller --parallel "$JOBS"

BINARY="$BUILD_DIR/controller/circuit_controller"
DESCRIPTION="$(file -b "$BINARY")"
printf 'ARM64 artifact: %s\n' "$DESCRIPTION"
if [[ "$DESCRIPTION" != *"aarch64"* || "$DESCRIPTION" != *"statically linked"* ]]; then
    printf 'Expected a statically linked AArch64 executable; refusing deployment.\n' >&2
    exit 1
fi

if [[ "$BUILD_ONLY" == true ]]; then
    printf 'Build-only mode: transfer was not attempted.\n'
    exit 0
fi

: "${TARGET_SSH_USER:?Set TARGET_SSH_USER before deployment}"
: "${TARGET_SSH_ADDRESS:?Set TARGET_SSH_ADDRESS before deployment}"
TARGET="${TARGET_SSH_USER}@${TARGET_SSH_ADDRESS}"
REMOTE_BINARY_PATH="${REMOTE_BINARY_PATH:-/home/$TARGET_SSH_USER/circuit_controller}"
SSH_CONNECT_TIMEOUT="${SSH_CONNECT_TIMEOUT:-5}"

printf 'Checking SSH reachability of %s before SCP...\n' "$TARGET"
if ! ssh -o BatchMode=yes -o "ConnectTimeout=$SSH_CONNECT_TIMEOUT" "$TARGET" true; then
    printf 'Target is unreachable or SSH authentication failed; SCP was not attempted.\n' >&2
    exit 3
fi

scp -o BatchMode=yes -o "ConnectTimeout=$SSH_CONNECT_TIMEOUT" \
    "$BINARY" "$TARGET:$REMOTE_BINARY_PATH"
printf 'Transferred %s to %s:%s\n' "$BINARY" "$TARGET" "$REMOTE_BINARY_PATH"
