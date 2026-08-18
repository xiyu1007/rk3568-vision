#!/usr/bin/env bash
# ============================================================================
# fetch_deps.sh — 拉取 RKNN 第三方依赖到 third_lib/
# ============================================================================
#
# 依赖清单：
#   1. third_lib/librknn_api/include/rknn_api.h   RKNN C API 头文件（rknpu2 SDK）
#   2. third_lib/librknn_api/aarch64/librknnrt.so RKNN 运行时库（2.3.2，aarch64）
#   3. third_lib/mediamtx/                        mediamtx RTMP 推流服务器（单二进制）
#
# 说明：这些依赖已入库 third_lib/（随仓库分发）；本脚本仅在缺失时兜底联网拉取
#       （用于精简克隆或文件被误删的场景）。配置解析用自研 YAML 解析器，无额外依赖。
# ============================================================================

set -euo pipefail
cd "$(dirname "$0")/.."

RKNN_DIR="third_lib/librknn_api"
mkdir -p "$RKNN_DIR/include" "$RKNN_DIR/aarch64"

# ---------------------------------------------------------------------------
# 1. rknn_api.h（rknpu2 SDK，来自 airockchip/rknn-toolkit2）
#    已在仓库里（third_lib 已入库），仅在缺失时才联网拉取。
# ---------------------------------------------------------------------------
if [ -f "$RKNN_DIR/include/rknn_api.h" ]; then
    echo "[OK] rknn_api.h present"
else
    echo "fetching rknn_api.h ..."
    curl -fsSL --retry 2 --connect-timeout 5 --max-time 60 -o "$RKNN_DIR/include/rknn_api.h" \
        "https://cdn.jsdelivr.net/gh/airockchip/rknn-toolkit2@master/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h" \
        || echo "[FAIL] 请手动放置 rknn_api.h 到 $RKNN_DIR/include/"
fi

# ---------------------------------------------------------------------------
# 2. librknnrt.so（aarch64 运行时，2.3.2，从 GitHub raw 下载）
#    注意：板端模型用 rknn-toolkit2 2.3.2 转换，需 2.x 运行时（旧 1.4/1.5 会加载失败 -6）
# ---------------------------------------------------------------------------
if [ -f "$RKNN_DIR/aarch64/librknnrt.so" ]; then
    echo "[OK] librknnrt.so present"
else
    echo "fetching librknnrt.so (2.3.2) ..."
    curl -fSL --retry 2 --connect-timeout 5 --max-time 60 -o "$RKNN_DIR/aarch64/librknnrt.so" \
        "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/master/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so" \
        || echo "[FAIL] 请手动放置 librknnrt.so 到 $RKNN_DIR/aarch64/"
fi

# ---------------------------------------------------------------------------
# 3. mediamtx（RTMP 推流服务器，单二进制，来自 bluenviron/mediamtx）
# ---------------------------------------------------------------------------
MEDIAMTX_DIR="third_lib/mediamtx"
MEDIAMTX_VERSION="v1.9.3"
MEDIAMTX_TARBALL="/tmp/mediamtx_${MEDIAMTX_VERSION}.tar.gz"

if [ -f "$MEDIAMTX_DIR/mediamtx" ]; then
    echo "[OK] mediamtx present"
else
    echo "fetching mediamtx ${MEDIAMTX_VERSION} ..."
    mkdir -p "$MEDIAMTX_DIR"
    curl -fSL --retry 2 --connect-timeout 5 --max-time 120 -o "$MEDIAMTX_TARBALL" \
        "https://github.com/bluenviron/mediamtx/releases/download/${MEDIAMTX_VERSION}/mediamtx_${MEDIAMTX_VERSION}_linux_arm64v8.tar.gz" \
        && tar -xzf "$MEDIAMTX_TARBALL" -C "$MEDIAMTX_DIR" \
        && chmod +x "$MEDIAMTX_DIR/mediamtx" \
        && rm -f "$MEDIAMTX_TARBALL" \
        || echo "[FAIL] 请手动下载 mediamtx 到 $MEDIAMTX_DIR/"
fi

echo ""
echo "=== dependencies ready ==="
