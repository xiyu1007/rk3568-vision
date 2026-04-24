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

# for aarch64
# TOOLCHAIN=/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf
# TOOLCHAIN=/usr/bin/gcc
# 指定 ARM64 交叉编译工具链路径（RK3568 用）
# 这是前缀，不是完整 gcc 命令（后面会拼 -gcc / -g++）

OUTPUT_DIR=${ROOT_PWD}/output/
rm -rf ${OUTPUT_DIR}
mkdir -p ${OUTPUT_DIR}

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

make -j4
# 开始编译（使用 4 线程并行编译，加快速度）

make install

cd -
# 返回到执行脚本之前的目录