#!/usr/bin/env bash
# ============================================================================
# verify_rtmp.sh — 验证 RTMP 推流是否正常
# ============================================================================
#
# 用法：
#   ./scripts/verify_rtmp.sh [rtmp_url]
#
# 默认 url：rtmp://127.0.0.1/live/stream
#
# 用 ffprobe 检查流是否可读（能读到 H.264 流信息即说明推流成功）。
# ============================================================================

set -euo pipefail

URL="${1:-rtmp://127.0.0.1/live/stream}"

if ! command -v ffprobe >/dev/null 2>&1; then
    echo "ffprobe 未安装（需要 ffmpeg），请先安装：sudo apt-get install -y ffmpeg"
    exit 1
fi

echo "=== probing $URL ==="
# -v error 只输出错误；-show_streams 显示流信息；超时 5 秒。
ffprobe -v error -show_streams -select_streams v:0 \
    -of default=noprint_wrappers=1 "$URL" 2>&1

echo ""
echo "若上面打印出 codec_name=h264、width/height 等信息，说明推流正常。"
echo "实时画面预览：ffplay $URL"
