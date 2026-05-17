#!/bin/bash
set -e
cd "$(dirname "$0")"
BIN="build/rk3568_vision"
CFG="config/default.yaml"

BUILD=1 RUN=1 CLEAN=0 DISP=1
while getopts "brcd:W:H:f:iDh" opt; do
    case $opt in
        b) BUILD=1; RUN=0 ;;
        r) BUILD=0; RUN=1 ;;
        c) CLEAN=1 ;;
        d) CAM_DEV="$OPTARG" ;;
        W) CAM_W="$OPTARG" ;;
        H) CAM_H="$OPTARG" ;;
        f) CAM_FPS="$OPTARG" ;;
        i) INF=1 ;;
        D) DISP=1 ;;
        h) echo "Usage: $0 [-b|-r|-c] [-d DEV] [-W W] [-H H] [-i] [-D]"; exit 0 ;;
    esac
done

if [ $BUILD -eq 1 ]; then
    echo "=== Building rk3568_vision v3.0.0 ==="
    [ $CLEAN -eq 1 ] && rm -rf build
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
    echo "[OK] Build complete"
fi

if [ $RUN -eq 1 ]; then
    cd "$(dirname "$0")"
    mkdir -p log
    ARGS="-c $CFG"
    [ -n "$CAM_DEV" ] && ARGS="$ARGS -d $CAM_DEV"
    [ -n "$CAM_W" ]   && ARGS="$ARGS -W $CAM_W"
    [ -n "$CAM_H" ]   && ARGS="$ARGS -H $CAM_H"
    [ -n "$CAM_FPS" ] && ARGS="$ARGS -f $CAM_FPS"
    [ "$INF" != "1" ]  && ARGS="$ARGS -n"
    [ "$DISP" != "1" ] && ARGS="$ARGS -N"
    echo "=== Running $BIN $ARGS ==="
    exec "$BIN" $ARGS
fi
