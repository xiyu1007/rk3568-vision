#!/bin/bash

set -e  # 遇到错误立即退出

# 配置
BUILD_DIR="build_opencv"
INSTALL_DIR="third_lib/opencv"
OPENCV_SRC="opencv"

# 清理
rm -rf $BUILD_DIR
mkdir $BUILD_DIR
cd $BUILD_DIR

cmake ../$OPENCV_SRC \
    -DCMAKE_INSTALL_PREFIX=$(pwd)/../$INSTALL_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_LIST=core,imgproc,videoio,highgui,imgcodecs \
    -DOPENCV_DOWNLOAD_DISABLE=ON \
    -DWITH_V4L=ON \
    -DWITH_FFMPEG=ON \
    -DWITH_GSTREAMER=ON \
    -DBUILD_opencv_highgui=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DWITH_GTK=ON \
    -DWITH_GTK_2_X=OFF \
    -DBUILD_PNG=ON \
    -DWITH_PNG=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DWITH_IPP=OFF \
    -DWITH_IPP_A=OFF \
    -DWITH_ADE=OFF \
    -DBUILD_opencv_gapi=OFF \
    -DPNG_ARM_NEON=OFF \
    -DENABLE_NEON=OFF

# 编译
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