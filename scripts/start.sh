#!/usr/bin/env bash
# ============================================================================
# start.sh — 一键启动（mediamtx 推流服务器 + rk3568_vision 应用）
# ============================================================================
#
# 用途：先确保 mediamtx（RTMP 推流服务器）在运行，再启动 rk3568_vision 推流。
#       不做自动重启（进程退出即结束），便于前台查看日志/调试。
#
# 用法：
#   ./scripts/start.sh                                 # mp4 输入推流（默认 conf/test_mp4.yaml）
#   ./scripts/start.sh -c conf/camera_push.yaml        # 摄像头纯推流（不推理）
#   ./scripts/start.sh -c conf/default.yaml -d /dev/video0   # 摄像头检测推流
#
# 注意：后面的参数原样透传给 rk3568_vision（-c/-d/-W/-H/-f/-s 等）。
# ============================================================================

set -e

# 项目根目录（脚本在 scripts/ 下，上级即项目根）。
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

# ---------------------------------------------------------------------------
# 1. 启动 mediamtx（若已在运行则跳过）
# ---------------------------------------------------------------------------
if pgrep -x mediamtx > /dev/null 2>&1; then
    echo "[OK] mediamtx 已在运行"
else
    echo "==> 启动 mediamtx 推流服务器"
    (cd third_lib/mediamtx && nohup ./mediamtx > mediamtx.log 2>&1 &)
    sleep 2
    if pgrep -x mediamtx > /dev/null 2>&1; then
        echo "[OK] mediamtx 已启动（监听 1935）"
    else
        echo "[ERROR] mediamtx 启动失败，请检查 third_lib/mediamtx/mediamtx.log"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# 2. 启动应用（参数透传），前台运行，Ctrl+C 停止
# ---------------------------------------------------------------------------
echo "==> 启动 rk3568_vision（参数：$*）"
exec ./output/rk3568_vision "$@"
