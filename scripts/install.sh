#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="samplerobot"

cd "$REPO_ROOT"
if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing AutoSample Module ==="
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/tools/$MODULE_ID"
scp -r dist/$MODULE_ID/* ableton@move.local:/data/UserData/move-anything/modules/tools/$MODULE_ID/
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/tools/$MODULE_ID"
echo "=== Install Complete ==="
