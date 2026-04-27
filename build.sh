#!/bin/bash
# 指定使用 bash 解释器执行这个脚本

clear

# 开启“遇错即停”模式：任何命令执行失败，脚本立即退出，防止继续执行产生错误结果
set -e

# ROOT_PWD=$( cd "$( dirname $0 )" && cd -P "$( dirname "$SOURCE" )" && pwd )
ROOT_PWD=$(cd "$(dirname "$0")" && pwd)

# 获取当前脚本所在的工程根目录
# dirname $0：获取脚本所在路径
# cd -P：进入真实路径（解析软链接）
# pwd：输出绝对路径

LOG_DIR=${ROOT_PWD}/share/bin
rm -rf ${LOG_DIR}
mkdir -p ${LOG_DIR}


LOG_DIR=${ROOT_PWD}/tmp/
rm -rf ${LOG_DIR}
mkdir -p ${LOG_DIR}

# build
BUILD_DIR=${ROOT_PWD}/build/
rm -rf ${BUILD_DIR}
mkdir -p ${BUILD_DIR}

cd ${BUILD_DIR}
# 进入构建目录（所有编译输出都在这里）

# 配置 CMake 工程，指定“源码目录”为 ROOT_PWD（CMakeLists.txt 所在目录）
# 指定 C 编译器：aarch64-none-linux-gnu-gcc（交叉编译）
# 指定 C++ 编译器：aarch64-none-linux-gnu-g++

cmake ${ROOT_PWD} 
# cmake ${ROOT_PWD} \
#     -DCMAKE_C_COMPILER=${TOOLCHAIN}-gcc \
#     -DCMAKE_CXX_COMPILER=${TOOLCHAIN}-g++

# make VERBOSE=1

make -j$(nproc)
# 开始编译（使用 4 线程并行编译，加快速度）

make install

cd -
# 返回到执行脚本之前的目录


# SSH X11 转发: (ssh -X user@host , ssh -Y user@host) SSH 自动分配（X11 forwarding）
# export DISPLAY=localhost:10.0
# wsl:  本地 X Server（Xming） WSL 自动映射到 Windows 的 X Server（如 Xming）
# export DISPLAY=:0 

# 通用：
# 判断是否在 WSL
if grep -qi WSL /proc/version 2>/dev/null; then
    # WSL 环境
    echo "WSL detected, set DISPLAY=:0"
    export DISPLAY=:0
# 本地 Linux（物理机）
else
    if [ -z "$DISPLAY" ]; then
        echo "Local machine with no DISPLAY, fallback to :0"
        export localhost:10.0
    else
        echo "Local machine, using DISPLAY: $DISPLAY"
    fi
fi