#!/bin/bash
# ============================================================================
# run.bash — RK3568 Vision Pipeline 一键构建+运行脚本
# ============================================================================
#
# **功能**：编译项目并启动流水线，支持命令行参数覆盖配置
#
# **使用示例**：
#   ./run.bash                    # 构建 + 运行（默认配置）
#   ./run.bash -b                 # 仅构建（不运行）
#   ./run.bash -r                 # 仅运行（不构建）
#   ./run.bash -c                 # 清理构建目录后重新构建
#   ./run.bash -d /dev/video1     # 使用 /dev/video1 设备
#   ./run.bash -W 1920 -H 1080 -f 30   # 指定分辨率 1920×1080@30fps
#   ./run.bash -D                 # 启用显示（-D 标志）
#   ./run.bash -h                 # 显示帮助
#
# **构建流程**：
#   1. 创建 build 目录（如果不存在）
#   2. 运行 cmake .. -DCMAKE_BUILD_TYPE=Release
#   3. 运行 make -j$(nproc)（并行编译，使用所有 CPU 核心）
#
# **运行流程**：
#   1. 创建 log 目录（如果不存在）
#   2. 启动 rk3568_vision 可执行文件
#   3. 传递所有命令行参数
#
# **关键变量**：
#   BUILD=1：是否执行构建
#   RUN=1：是否执行运行
#   CLEAN=0：是否清理构建目录
#   INF=0/DISP=0：推理和显示的默认值（通过 -i/-D 启用）
# ============================================================================

set -e                    # 任何命令失败（返回非0）时立即退出
cd "$(dirname "$0")"       # 切换到脚本所在目录
BIN="build/rk3568_vision"  # 可执行文件路径
CFG="config/default.yaml"  # 默认配置文件路径

# 默认行为：构建 + 运行，不清理，不启用推理/显示的额外选项
BUILD=1 RUN=1 CLEAN=0 INF=0 DISP=0

# 解析命令行选项
while getopts "brcd:W:H:f:iDh" opt; do
    case $opt in
        b) BUILD=1; RUN=0 ;;     # -b：仅构建
        r) BUILD=0; RUN=1 ;;     # -r：仅运行
        c) CLEAN=1 ;;             # -c：清理构建目录
        d) CAM_DEV="$OPTARG" ;;  # -d DEV：指定 V4L2 设备节点
        W) CAM_W="$OPTARG" ;;    # -W W：指定采集宽度
        H) CAM_H="$OPTARG" ;;    # -H H：指定采集高度
        f) CAM_FPS="$OPTARG" ;;  # -f FPS：指定帧率
        i) INF=1 ;;              # -i：启用推理（-i 标志）
        D) DISP=1 ;;             # -D：启用显示
        h) echo "Usage: $0 [-b|-r|-c] [-d DEV] [-W W] [-H H] [-f FPS] [-i] [-D]"; exit 0 ;;
    esac
done

# ── 构建阶段 ────────────────────────────────────────────────────────────
if [ $BUILD -eq 1 ]; then
    echo "=== Building rk3568_vision v3.0.0 ==="
    [ $CLEAN -eq 1 ] && rm -rf build   # -c 参数：清理旧的构建产物
    mkdir -p build && cd build         # 创建并进入 build 目录（out-of-source 构建）
    cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
    # -j$(nproc)：并行编译，nproc 返回 CPU 核心数，最大化编译速度
    echo "[OK] Build complete"
fi

# ── 运行阶段 ────────────────────────────────────────────────────────────
if [ $RUN -eq 1 ]; then
    cd "$(dirname "$0")"        # 确保在项目根目录运行（从 build/ 返回）
    mkdir -p log                # 创建日志目录
    ARGS="-c $CFG"              # 基础参数：指定配置文件
    # 追加命令行覆盖参数
    [ -n "$CAM_DEV" ] && ARGS="$ARGS -d $CAM_DEV"
    [ -n "$CAM_W" ]   && ARGS="$ARGS -W $CAM_W"
    [ -n "$CAM_H" ]   && ARGS="$ARGS -H $CAM_H"
    [ -n "$CAM_FPS" ] && ARGS="$ARGS -f $CAM_FPS"
    [ "$INF" = "1" ]  && ARGS="$ARGS"   # 推理默认由配置文件中的 enabled 控制
    echo "=== Running $BIN $ARGS ==="
    exec "$BIN" $ARGS                    # exec 替换当前 bash 进程（节省内存）
fi

# ffplay rtmp://192.168.31.28/live/stream
