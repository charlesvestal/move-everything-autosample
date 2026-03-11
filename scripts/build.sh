#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="samplerobot"
IMAGE_NAME="move-anything-samplerobot-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Building Sample Robot Module (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        bash scripts/build.sh
    exit $?
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"
echo "=== Building Sample Robot Module ==="

mkdir -p build
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/samplerobot_plugin.c \
    src/dsp/wav_writer.c \
    src/dsp/silence_detect.c \
    src/dsp/sfz_writer.c \
    src/dsp/loop_finder.c \
    src/dsp/third_party/kissfft/kiss_fft.c \
    src/dsp/third_party/kissfft/kiss_fftr.c \
    -o build/dsp.so \
    -Isrc/dsp \
    -Isrc/dsp/third_party/kissfft \
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
