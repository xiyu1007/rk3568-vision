#!/bin/bash

# 配置
BUILD_DIR="build_opencv"
INSTALL_DIR="third_lib/opencv_rk"
OPENCV_SRC="opencv"

# 交叉编译工具链路径
TOOLCHAIN_PATH="/usr/local/arm64/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu"
CROSS_COMPILE="aarch64-none-linux-gnu"

set -e

# 清理环境变量（避免冲突）
unset CPLUS_INCLUDE_PATH
unset C_INCLUDE_PATH
unset LIBRARY_PATH
unset CPATH

# 清理构建目录
rm -rf $BUILD_DIR
mkdir $BUILD_DIR
cd $BUILD_DIR

# CMake 配置
cmake ../$OPENCV_SRC \
    -DCMAKE_INSTALL_PREFIX=$(pwd)/../$INSTALL_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc \
    -DCMAKE_CXX_COMPILER=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-g++ \
    -DCMAKE_SYSROOT=${TOOLCHAIN_PATH}/${CROSS_COMPILE}/libc \
    -DCMAKE_FIND_ROOT_PATH=${TOOLCHAIN_PATH}/${CROSS_COMPILE}/libc \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    \
    -DOPENCV_DOWNLOAD_DISABLE=ON \
    \
    -DBUILD_LIST=core,imgproc,videoio,highgui,imgcodecs \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_opencv_gapi=OFF \
    -DBUILD_opencv_highgui=ON \
    \
    -DWITH_V4L=ON \
    -DWITH_FFMPEG=OFF \
    -DWITH_GSTREAMER=OFF \
    -DWITH_GTK=OFF \
    -DBUILD_opencv_highgui=ON \
    -DWITH_GTK=ON \
    -DWITH_GTK_2_X=ON \
    -DWITH_IPP=OFF \
    -DWITH_ADE=OFF \
    \
    -DBUILD_ZLIB=ON \
    -DBUILD_JPEG=ON \
    -DBUILD_PNG=OFF \
    -DBUILD_TIFF=OFF \
    \
    -DWITH_PNG=OFF \
    -DWITH_JPEG=ON \
    -DWITH_TIFF=OFF \
    \
    -DENABLE_NEON=OFF \
    -DWITH_NEON=OFF \
    -DENABLE_VFPV3=OFF \
    -DCV_DISABLE_OPTIMIZATION=ON \
    -DCPU_BASELINE="ARMV8" \
    -DCPU_DISPATCH="" \
    \
    -DPNG_ARM_NEON=0 \
    -DCMAKE_C_FLAGS="-DPNG_NO_NEON -DPNG_ARM_NEON_OPT=0" \
    -DCMAKE_CXX_FLAGS="-DPNG_NO_NEON -DPNG_ARM_NEON_OPT=0"

# 编译（使用多线程）
CPU_CORES=$(nproc)
make -j${CPU_CORES}

# 安装
make install

cd -
echo "=========================================="
echo "OpenCV installed to: $(pwd)/$INSTALL_DIR"
echo "=========================================="

# 显示库文件
ls -la $(pwd)/$INSTALL_DIR/lib/