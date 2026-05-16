#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BINARY="${BUILD_DIR}/rk3568_vision"
CONFIG="${SCRIPT_DIR}/config/default.yaml"

# 默认参数
CAM_DEV="/dev/video0"
CAM_W="1920"
CAM_H="1080"
BUILD_ONLY=0
RUN_ONLY=0
CLEAN=0
ENABLE_INFER=0
ENABLE_DISPLAY=1

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "  -b          Only build"
    echo "  -r          Only run (skip build)"
    echo "  -c          Clean rebuild"
    echo "  -d DEV      Camera device (default: /dev/video0)"
    echo "  -W WIDTH    Width (default: 1920)"
    echo "  -H HEIGHT   Height (default: 1080)"
    echo "  -i          Enable inference"
    echo "  -D          Enable display"
    echo "  -h          This help"
    exit 0
}

while getopts "brcd:W:H:iDh" opt; do
    case $opt in
        b) BUILD_ONLY=1 ;;
        r) RUN_ONLY=1 ;;
        c) CLEAN=1 ;;
        d) CAM_DEV="$OPTARG" ;;
        W) CAM_W="$OPTARG" ;;
        H) CAM_H="$OPTARG" ;;
        i) ENABLE_INFER=1 ;;
        D) ENABLE_DISPLAY=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

# === 编译 ===
if [ $RUN_ONLY -eq 0 ]; then
    echo "========================================"
    echo " Building RK3568 Vision Pipeline"
    echo "========================================"

    [ $CLEAN -eq 1 ] && rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    HOST_ARCH=$(uname -m)
    if [ "$HOST_ARCH" = "aarch64" ]; then
        echo "[INFO] Native build on aarch64 (RK3568 board)"
        cd "$BUILD_DIR"
        cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release
    else
        echo "[INFO] Build on $HOST_ARCH (RKNN stubbed)"
        cd "$BUILD_DIR"
        cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release
    fi

    make -j$(nproc)
    echo "[OK] Build complete: $BINARY"
    echo ""
fi

# === 运行 ===
if [ $BUILD_ONLY -eq 0 ]; then
    echo "========================================"
    echo " Running RK3568 Vision Pipeline"
    echo "========================================"
    echo "  Camera:  $CAM_DEV ${CAM_W}x${CAM_H}"
    echo "  Config:  $CONFIG"
    echo "========================================"

    cd "$SCRIPT_DIR"
    mkdir -p log

    EXTRA=""
    [ $ENABLE_INFER -eq 0 ] && EXTRA="$EXTRA -n"
    [ $ENABLE_DISPLAY -eq 0 ] && EXTRA="$EXTRA -N"
    [ -z "$DISPLAY" ] && EXTRA="$EXTRA -N"

    exec "$BINARY" \
        -c "$CONFIG" \
        -d "$CAM_DEV" \
        -W "$CAM_W" \
        -H "$CAM_H" \
        $EXTRA
fi
