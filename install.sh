#!/usr/bin/env bash
# ============================================================================
# install.sh — 一键部署：检测环境 → 拉取依赖 → 编译 → 运行（仅板端）
# ============================================================================
#
# 自动完成：
#   1. 检测/安装编译依赖（aarch64：FFmpeg+RGA+MPP 开发库；x86：aarch64 交叉编译器）
#   2. 检测/拉取 third_lib（RKNN 运行时 + mediamtx，缺失时联网拉取）
#   3. 编译（aarch64 原生编译；x86 交叉编译出 aarch64 可执行文件）
#   4. 运行（仅 aarch64：启动 mediamtx + 推流应用，参数透传）
#
# x86 交叉编译：产物 output/rk3568_vision 为 aarch64 可执行文件，直接拷到板端运行。
# ============================================================================

set -e
cd "$(dirname "$0")"

ARCH="$(uname -m)"

run_sudo() { echo "${SUDO_PASS:-1}" | sudo -S "$@"; }

echo "============================================================"
echo " rk3568_vision 一键部署（$ARCH）"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. 编译依赖
# ---------------------------------------------------------------------------
if [ "$ARCH" = "aarch64" ]; then
    echo ""
    echo "==> [1/4] 检查编译依赖（FFmpeg + RGA + MPP）"
    if pkg-config --exists libavcodec libavformat libavutil libswscale librga rockchip_mpp; then
        echo "    编译依赖已就绪"
    else
        echo "    依赖缺失，开始安装 ..."
        run_sudo apt-get update -y
        run_sudo apt-get install -y \
            libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
            librga-dev librockchip-mpp-dev \
            || echo "    [WARN] 依赖安装失败（非 Rockchip 板端或软件源不可用）"
    fi
else
    echo ""
    echo "==> [1/4] 检查交叉编译工具链（aarch64-linux-gnu-g++）"
    if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        echo "    交叉编译器已就绪"
    else
        echo "    安装交叉编译器 ..."
        run_sudo apt-get update -y
        run_sudo apt-get install -y g++-aarch64-linux-gnu \
            || echo "    [WARN] 交叉编译器安装失败"
    fi
fi

# ---------------------------------------------------------------------------
# 2. third_lib（RKNN 运行时 + mediamtx）
# ---------------------------------------------------------------------------
echo ""
echo "==> [2/4] 检查 third_lib（RKNN 运行时 + mediamtx）"
./scripts/fetch_deps.sh

# ---------------------------------------------------------------------------
# 3. 编译
# ---------------------------------------------------------------------------
echo ""
echo "==> [3/4] 编译"
if [ "$ARCH" = "aarch64" ]; then
    make clean
    make
else
    # x86：交叉编译 aarch64（aarch64 头文件/库已入库在 third_lib/aarch64-sysroot）
    make clean
    make CROSS_COMPILE=aarch64-linux-gnu-
fi

# ---------------------------------------------------------------------------
# 4. 运行（仅 aarch64）
# ---------------------------------------------------------------------------
echo ""
if [ "$ARCH" = "aarch64" ]; then
    echo "==> [4/4] 启动运行（mediamtx + rk3568_vision）"
    exec ./scripts/start.sh "$@"
else
    echo "==> [4/4] 交叉编译完成：output/rk3568_vision（aarch64）"
    echo "    拷到板端运行：scp output/rk3568_vision rk3568:~/project/gx/rk3568-vision/output/"
fi
