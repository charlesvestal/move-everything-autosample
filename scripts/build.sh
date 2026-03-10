#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="samplerobot"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"
echo "=== Building Sample Robot Module ==="

mkdir -p build
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/samplerobot_plugin.c \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

rm -rf "dist/$MODULE_ID"
mkdir -p "dist/$MODULE_ID"

cp src/module.json "dist/$MODULE_ID/"
cp src/ui.js "dist/$MODULE_ID/"
cp build/dsp.so "dist/$MODULE_ID/"
[ -f src/help.json ] && cp src/help.json "dist/$MODULE_ID/"
chmod +x "dist/$MODULE_ID/dsp.so"

cd dist
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
