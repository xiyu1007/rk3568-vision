#!/usr/bin/env bash
# ============================================================================
# fetch_deps.sh — 拉取第三方依赖到 third_lib/
# ============================================================================
#
# 依赖清单：
#   1. third_lib/rknn/include/rknn_api.h   RKNN C API 头文件（rknpu2 SDK）
#   2. third_lib/rknn/aarch64/librknnrt.so RKNN 运行时库（aarch64）
#   3. third_lib/json/json.hpp             nlohmann/json 单头库
#
# 说明：这些依赖体积较大/涉及外部 SDK，不入库（.gitignore 忽略 third_lib/），
#       在全新环境上用本脚本一键拉取。
# ============================================================================

set -euo pipefail
cd "$(dirname "$0")/.."

RKNN_DIR="third_lib/rknn"
JSON_DIR="third_lib/json"
mkdir -p "$RKNN_DIR/include" "$RKNN_DIR/aarch64" "$JSON_DIR"

# ---------------------------------------------------------------------------
# 1. rknn_api.h（rknpu2 SDK，来自 airockchip/rknn-toolkit2）
#    优先 jsdelivr 镜像，失败回退 raw.githubusercontent。
# ---------------------------------------------------------------------------
fetch_header() {
    local urls=(
        "https://cdn.jsdelivr.net/gh/airockchip/rknn-toolkit2@master/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h"
        "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/master/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h"
    )
    for u in "${urls[@]}"; do
        echo "fetching rknn_api.h from $u"
        if curl -fsSL -o "$RKNN_DIR/include/rknn_api.h" "$u"; then
            echo "[OK] rknn_api.h ($(wc -l < "$RKNN_DIR/include/rknn_api.h") lines)"
            return 0
        fi
    done
    echo "[FAIL] cannot download rknn_api.h, please place it manually at $RKNN_DIR/include/"
    return 1
}

# ---------------------------------------------------------------------------
# 2. librknnrt.so（若本地不存在才下载；GitHub 大文件需手动放置）
# ---------------------------------------------------------------------------
if [ -f "$RKNN_DIR/aarch64/librknnrt.so" ]; then
    echo "[OK] librknnrt.so already present ($(du -h "$RKNN_DIR/aarch64/librknnrt.so" | cut -f1))"
else
    echo "[WARN] librknnrt.so not found."
    echo "       请从 rknpu2 SDK 拷贝到 $RKNN_DIR/aarch64/librknnrt.so"
    echo "       或从 rknn-toolkit2 发布页下载。"
fi

# ---------------------------------------------------------------------------
# 3. nlohmann/json.hpp
# ---------------------------------------------------------------------------
echo "fetching json.hpp ..."
curl -fsSL -o "$JSON_DIR/json.hpp" \
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
echo "[OK] json.hpp ($(du -h "$JSON_DIR/json.hpp" | cut -f1))"

echo ""
echo "=== dependencies ready ==="
