#!/bin/bash
# Apply positron patches to third-party submodules.
# Run after `git submodule update --init --recursive`.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[patches] Applying BlackBone patch..."
cd "$ROOT/third_party/Blackbone"
patch -p1 --forward < "$SCRIPT_DIR/blackbone.patch" || true
echo "[patches] Done."
