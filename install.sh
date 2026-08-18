#!/usr/bin/env bash
# ============================================================================
# install — 一键部署：检测环境 → 拉取依赖 → 编译 → 运行（仅板端）
# ============================================================================
#
# 自动完成：
#   1. 检测/安装编译依赖（aarch64：FFmpeg + RGA + MPP 开发库，缺则 apt 装）
#   2. 检测/拉取 third_lib（RKNN 运行时 + mediamtx，缺失时联网拉取）
#   3. 编译（aarch64 生成可执行文件；x86_64 仅编译检查，不运行）
#   4. 运行（仅 aarch64：启动 mediamtx + 推流应用，参数透传）
#
# 用法：
#   ./install                                          # 默认 conf/default.yaml
#   ./install -c conf/camera_push.yaml -d /dev/video0  # 摄像头纯推流（不推理）
#   ./install -c conf/test_mp4.yaml                    # mp4 联调
# ============================================================================

set -e
cd "$(dirname "$0")"

ARCH="$(uname -m)"

# sudo 密码（默认 1，可用 SUDO_PASS 环境变量覆盖）。
run_sudo() { echo "${SUDO_PASS:-1}" | sudo -S "$@"; }

echo "============================================================"
echo " rk3568_vision 一键部署（$ARCH）"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. 编译依赖（仅 aarch64 需要；x86 只做编译检查，不装依赖）
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
    echo "==> [1/4] x86_64：跳过依赖安装（仅编译检查）"
fi

# ---------------------------------------------------------------------------
# 2. third_lib（RKNN 运行时 + mediamtx，缺失时联网拉取）
# ---------------------------------------------------------------------------
echo ""
echo "==> [2/4] 检查 third_lib（RKNN 运行时 + mediamtx）"
./scripts/fetch_deps.sh

# ---------------------------------------------------------------------------
# 3. 编译
# ---------------------------------------------------------------------------
echo ""
echo "==> [3/4] 编译"
make clean
make

# ---------------------------------------------------------------------------
# 4. 运行（仅 aarch64）
# ---------------------------------------------------------------------------
echo ""
if [ "$ARCH" = "aarch64" ]; then
    echo "==> [4/4] 启动运行（mediamtx + rk3568_vision）"
    exec ./scripts/start.sh "$@"
else
    echo "==> [4/4] x86_64：编译检查完成，不运行（真实推理需板端 NPU）"
fi
