#!/usr/bin/env bash
# ============================================================================
# fetch_deps.sh — 拉取 RKNN 第三方依赖到 third_lib/
# ============================================================================
#
# 依赖清单：
#   1. third_lib/librknn_api/include/rknn_api.h   RKNN C API 头文件（rknpu2 SDK）
#   2. third_lib/librknn_api/aarch64/librknnrt.so RKNN 运行时库（aarch64）
#
# 说明：这些依赖体积较大/涉及外部 SDK，不入库（.gitignore 忽略 third_lib/），
#       在全新环境上用本脚本一键拉取。配置解析用自研 YAML 解析器，无额外依赖。
# ============================================================================

set -euo pipefail
cd "$(dirname "$0")/.."

RKNN_DIR="third_lib/librknn_api"
mkdir -p "$RKNN_DIR/include" "$RKNN_DIR/aarch64"

# ---------------------------------------------------------------------------
# 1. rknn_api.h（rknpu2 SDK，来自 airockchip/rknn-toolkit2）
# ---------------------------------------------------------------------------
echo "fetching rknn_api.h ..."
curl -fsSL --retry 3 -o "$RKNN_DIR/include/rknn_api.h" \
    "https://cdn.jsdelivr.net/gh/airockchip/rknn-toolkit2@master/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h" \
    || echo "[FAIL] 请手动放置 rknn_api.h 到 $RKNN_DIR/include/"
[ -f "$RKNN_DIR/include/rknn_api.h" ] && echo "[OK] rknn_api.h ($(wc -l < "$RKNN_DIR/include/rknn_api.h") lines)"

# ---------------------------------------------------------------------------
# 2. librknnrt.so（aarch64 运行时，大文件，通常需手动放置）
# ---------------------------------------------------------------------------
if [ -f "$RKNN_DIR/aarch64/librknnrt.so" ]; then
    echo "[OK] librknnrt.so present ($(du -h "$RKNN_DIR/aarch64/librknnrt.so" | cut -f1))"
else
    echo "[WARN] librknnrt.so not found，请从 rknpu2 SDK 拷贝到 $RKNN_DIR/aarch64/"
fi

echo ""
echo "=== dependencies ready ==="
