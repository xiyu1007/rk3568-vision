#!/bin/bash

# 配置
BUILD_DIR="build_opencv"
INSTALL_DIR="third_lib/opencv"
OPENCV_SRC="opencv"

# 交叉编译工具链路径
# TOOLCHAIN_PATH="/usr/local/arm64/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu"
# CROSS_COMPILE="aarch64-none-linux-gnu"

# set(TOOLCHAIN_PATH /usr/local/arm64/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin)
# set(CROSS_COMPILE aarch64-none-linux-gnu)
# # 工具链路径
# set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/${CROSS_COMPILE}-gcc)
# set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PATH}/${CROSS_COMPILE}-g++)

unset CPLUS_INCLUDE_PATH
unset C_INCLUDE_PATH
unset LIBRARY_PATH
unset CPATH

# 清理
rm -rf $BUILD_DIR
mkdir $BUILD_DIR
cd $BUILD_DIR

// 把这里加进去给rk3568编译
# -DCMAKE_C_COMPILER=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc \
# -DCMAKE_CXX_COMPILER=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-g++ \
# -DCMAKE_SYSROOT=${TOOLCHAIN_PATH}/${CROSS_COMPILE}/libc \

cmake ../$OPENCV_SRC \
    -DCMAKE_INSTALL_PREFIX=$(pwd)/../$INSTALL_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_LIST=core,imgproc,videoio,highgui,imgcodecs \
    -DOPENCV_DOWNLOAD_DISABLE=ON \
    -DWITH_V4L=ON \
    -DWITH_FFMPEG=ON \
    -DWITH_GSTREAMER=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_PNG=ON \
    -DWITH_PNG=ON \
    -DWITH_IPP=OFF \
    -DWITH_IPP_A=OFF \
    -DWITH_ADE=OFF \
    -DBUILD_opencv_gapi=OFF \
    -DPNG_ARM_NEON=0 \
    -DENABLE_NEON=OFF

# 编译
make -j8

# 安装
make install

cd -
echo "OpenCV installed to: $(pwd)/$INSTALL_DIR"