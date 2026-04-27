
clear

set -e  # 遇到错误立即退出

# 1. 进入 nginx 源码目录
cd nginx-1.24.0

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)

INSTALL_DIR=${ROOT_PWD}/third_lib/nginx_rk/install

TOOLCHAIN_PATH="/usr/local/arm64/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu"
CROSS_COMPILE="aarch64-linux-gnu"

echo ${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc

# 3. 配置
./configure \
    --prefix=${INSTALL_DIR} \
    --with-cc=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc \
    --with-cpp=${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-cpp \
    --with-http_ssl_module \
    --with-http_mp4_module \
    --with-http_v2_module \
    --without-http_upstream_zone_module \
    --add-module=${ROOT_PWD}/nginx-rtmp-module

# 2. 创建安装目录
mkdir -p ${INSTALL_DIR}

# 4. 编译（RK3568 使用 4 核）
CPU_CORES=$(nproc)
make -j${CPU_CORES}

# 5. 安装
make install

cd -
cd ${INSTALL_DIR}
# 6. 测试
echo "===================================================="
./sbin/nginx -t
echo ""
./sbin/nginx -V  # 查看编译配置

cd -
echo "===================================================="