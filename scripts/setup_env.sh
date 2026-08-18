#!/usr/bin/env bash
# ============================================================================
# setup_env.sh — 一键部署环境脚本（全新 RK3568 板端）
# ============================================================================
#
# 用途：在全新的 RK3568 板端（Ubuntu 22.04 aarch64）上快速部署编译运行环境，
#       让 rk3568_vision 可以直接运行。换另一块开发板时，把项目同步过去后
#       执行本脚本即可。
#
# 部署内容：
#   1. 安装编译依赖（FFmpeg 开发库）
#   2. 确认 RKNN 运行时就绪（third_lib 的 2.3.2，程序经 rpath 引用，不替换系统 so）
#   3. 部署 mediamtx 推流服务器（third_lib/mediamtx，补执行权限）
#   4. 编译项目（生成 output/rk3568_vision）
#
# 用法：
#   sudo 密码默认 1（可用 SUDO_PASS 环境变量覆盖）：
#     SUDO_PASS=yourpass ./scripts/setup_env.sh
#   或直接：
#     ./scripts/setup_env.sh
# ============================================================================

set -e

# 项目根目录（脚本在 scripts/ 下，上级即项目根）。
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

# sudo 密码（默认 1，可用环境变量覆盖）。
SUDO_PASS="${SUDO_PASS:-1}"

# 用 sudo 执行命令（自动输入密码）。
run_sudo() {
    echo "$SUDO_PASS" | sudo -S "$@"
}

echo "============================================================"
echo " rk3568_vision 环境部署"
echo " 项目目录: $PROJECT_DIR"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. 安装编译依赖（FFmpeg 开发库 + RGA + MPP 硬件编码库）
#    注：librga-dev / librockchip-mpp-dev 是 Rockchip 板端仓库自带（LubanCat 镜像
#    通常已预装，此处安装仅为全新/精简镜像兜底；普通 x86 机器无此仓库会失败，
#    但 x86 仅做编译检查不链接，可忽略）。
# ---------------------------------------------------------------------------
echo ""
echo "==> [1/4] 安装编译依赖（FFmpeg + RGA + MPP）"
run_sudo apt-get update -y
run_sudo apt-get install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    librga-dev \
    librockchip-mpp-dev || echo "    [WARN] RGA/MPP 开发库安装失败（x86 编译检查或非 Rockchip 板端可忽略）"

# ---------------------------------------------------------------------------
# 2. 确认 RKNN 运行时就绪（程序经 rpath 引用 third_lib，不替换系统 /lib）
# ---------------------------------------------------------------------------
echo ""
echo "==> [2/4] 确认 RKNN 运行时"
RKNN_SO="third_lib/librknn_api/aarch64/librknnrt.so"
if [ -f "$RKNN_SO" ]; then
    echo "    third_lib librknnrt.so 已就绪（$(strings "$RKNN_SO" | grep -i 'version' | head -1)）"
    echo "    程序经 Makefile rpath 引用此版本，系统 /lib/librknnrt.so 保持默认不修改"
else
    echo "    [WARN] 未找到 $RKNN_SO，请先执行 ./scripts/fetch_deps.sh 拉取"
fi

# ---------------------------------------------------------------------------
# 3. 部署 mediamtx 推流服务器（仅补执行权限，启动见 scripts/start.sh）
# ---------------------------------------------------------------------------
echo ""
echo "==> [3/4] 部署 mediamtx 推流服务器"
MEDIAMTX="third_lib/mediamtx/mediamtx"
if [ -f "$MEDIAMTX" ]; then
    chmod +x "$MEDIAMTX"
    echo "    mediamtx 已就绪（third_lib/mediamtx/mediamtx）"
else
    echo "    [WARN] 未找到 $MEDIAMTX，请从 GitHub 下载放置到 third_lib/mediamtx/"
fi

# ---------------------------------------------------------------------------
# 4. 编译项目
# ---------------------------------------------------------------------------
echo ""
echo "==> [4/4] 编译项目"
make clean
make

echo ""
echo "============================================================"
echo " 部署完成"
echo "============================================================"
echo ""
echo " 启动 mediamtx（推流服务器）："
echo "   cd third_lib/mediamtx && ./mediamtx &"
echo ""
echo " 运行（mp4 输入，无摄像头时）："
echo "   ./output/rk3568_vision -c conf/test_mp4.yaml"
echo ""
echo " 运行（摄像头输入）："
echo "   ./output/rk3568_vision -c conf/default.yaml -d /dev/video0"
echo ""
