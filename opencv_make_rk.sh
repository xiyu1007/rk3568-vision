#!/bin/bash


# ================= 配置区 =================
OPENCV_SRC="opencv"
BUILD_DIR="build_opencv"
INSTALL_DIR="$(pwd)/third_lib/opencv"  

TOOLCHAIN_PATH="/usr/local/arm64/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu"
SYSROOT="/home/gx/linux/rk3568/rk3568-vision/sysroot-glibc-linaro-2.25-2019.12-aarch64-linux-gnu"
CROSS_COMPILE="aarch64-linux-gnu"

# 清理旧构建
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 【关键】强制 pkg-config 只在 sysroot 中搜索依赖（解决 GTK 找 x86 的问题）
export PKG_CONFIG_LIBDIR="${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"

cmake ../"$OPENCV_SRC" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc" \
    -DCMAKE_CXX_COMPILER="${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-g++" \
    -DCMAKE_SYSROOT="${SYSROOT}" \
    -DCMAKE_FIND_ROOT_PATH="${SYSROOT}" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DBUILD_LIST=core,imgproc,videoio,highgui,imgcodecs \
    -DOPENCV_DOWNLOAD_DISABLE=ON \
    -DWITH_V4L=ON \
    -DWITH_FFMPEG=ON \
    -DWITH_GSTREAMER=ON \
    -DWITH_GTK=ON \
    -DWITH_GTK_2_X=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_PNG=ON \
    -DWITH_PNG=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DWITH_IPP=OFF \
    -DWITH_IPP_A=OFF \
    -DWITH_ADE=OFF \
    -DBUILD_opencv_gapi=OFF

# 编译
make -j$(nproc)

# 安装
make install

cd ..
echo "=========================================="
echo " OpenCV installed to: $INSTALL_DIR"
echo "=========================================="
ls -la "$INSTALL_DIR/lib/"