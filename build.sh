#!/usr/bin/env bash
# ============================================================================
# build.sh — 一键构建 / 运行 / 部署脚本
# ============================================================================
#
# 用法：
#   ./build.sh build              # 构建（自动检测平台）
#   ./build.sh run [APP_ARGS]     # 构建(如需) + 运行（默认含推流）
#   ./build.sh run --no-stream    # 运行但不推流
#   ./build.sh clean              # 清理构建目录
#   ./build.sh fetch-deps         # 拉取第三方依赖（RKNN/JSON）
#   ./build.sh sync <rk_host>     # 同步产物到 RK 板（scp）
#   ./build.sh test-rtmp          # 启动本地 nginx-rtmp 并提示拉流验证
#   ./build.sh help               # 显示帮助
#
# 平台说明：
#   - aarch64（RK3568 板上）：原生构建，链接真实 RKNN + h264_rkmpp
#   - x86_64（WSL 开发机）  ：mock 推理 + libx264 软编，用于无硬件联调
# ============================================================================

set -euo pipefail

# 切到脚本所在目录（项目根目录）。
cd "$(dirname "$0")"

PROJECT="rk3568_vision"
BUILD_DIR="build"
BIN="$BUILD_DIR/$PROJECT"
CFG="conf/default.json"
RK_HOST=""                     # sync 目标（如 root@192.168.1.100）

# ---------------------------------------------------------------------------
# 检测目标架构
# ---------------------------------------------------------------------------
detect_arch() {
    case "$(uname -m)" in
        aarch64|arm64) echo "aarch64" ;;
        x86_64|amd64)  echo "x86_64" ;;
        *)             echo "unknown" ;;
    esac
}

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
do_build() {
    local arch
    arch="$(detect_arch)"
    echo "=== Building $PROJECT ($arch) ==="

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    if [ "$arch" = "aarch64" ]; then
        cmake .. -DCMAKE_BUILD_TYPE=Release
    else
        cmake .. -DCMAKE_BUILD_TYPE=Release
    fi
    make -j"$(nproc)"
    echo "[OK] build complete: $BIN"
}

# ---------------------------------------------------------------------------
# 运行
# ---------------------------------------------------------------------------
do_run() {
    if [ ! -x "$BIN" ]; then
        do_build
    fi
    mkdir -p log output
    echo "=== Running $BIN -c $CFG $* ==="
    exec "$BIN" -c "$CFG" "$@"
}

# ---------------------------------------------------------------------------
# 清理
# ---------------------------------------------------------------------------
do_clean() {
    rm -rf "$BUILD_DIR"
    echo "[OK] removed $BUILD_DIR"
}

# ---------------------------------------------------------------------------
# 拉取依赖
# ---------------------------------------------------------------------------
do_fetch_deps() {
    if [ -x scripts/fetch_deps.sh ]; then
        bash scripts/fetch_deps.sh
    else
        echo "scripts/fetch_deps.sh not found"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# 同步到 RK 板
# ---------------------------------------------------------------------------
do_sync() {
    if [ -z "$RK_HOST" ]; then
        echo "usage: $0 sync <rk_host>  (e.g. $0 sync root@192.168.1.100)"
        exit 1
    fi
    do_build
    echo "=== syncing to $RK_HOST:/home/gx/linux/rk3568/rk3568-vision ==="
    scp "$BIN" "$RK_HOST:/home/gx/linux/rk3568/rk3568-vision/$PROJECT"
    scp -r conf model "$RK_HOST:/home/gx/linux/rk3568/rk3568-vision/"
    echo "[OK] synced"
}

# ---------------------------------------------------------------------------
# 本地 RTMP 验证
# ---------------------------------------------------------------------------
do_test_rtmp() {
    echo "=== 本地 RTMP 推流验证 ==="
    echo "1. 启动 nginx-rtmp（如已安装）："
    echo "     sudo nginx -c $(pwd)/scripts/nginx.conf   # 或使用系统 nginx"
    echo "2. 运行推流："
    echo "     ./build.sh run -s rtmp://127.0.0.1/live/stream"
    echo "3. 另开终端拉流验证："
    echo "     ffplay rtmp://127.0.0.1/live/stream"
    echo ""
    echo "（也可用 ffprobe 检查流信息：ffprobe rtmp://127.0.0.1/live/stream）"
}

# ---------------------------------------------------------------------------
# 帮助
# ---------------------------------------------------------------------------
do_help() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------------------
# 主分发
# ---------------------------------------------------------------------------
case "${1:-help}" in
    build)      shift; do_build "$@" ;;
    run)        shift; do_run "$@" ;;
    clean)      do_clean ;;
    fetch-deps) do_fetch_deps ;;
    sync)       shift; RK_HOST="${1:-}"; shift 2>/dev/null || true; do_sync ;;
    test-rtmp)  do_test_rtmp ;;
    help|-h|--help) do_help ;;
    *)
        echo "unknown command: $1" >&2
        do_help
        exit 1
        ;;
esac
