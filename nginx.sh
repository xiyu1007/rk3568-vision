
clear

# 1. 进入 nginx 源码目录
cd nginx-1.24.0

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)
INSTALL_DIR=${ROOT_PWD}/third_lib/nginx-1.24.0/install

# 2. 创建安装目录
mkdir -p ${INSTALL_DIR}

# 3. 配置
./configure \
    --prefix=${INSTALL_DIR} \
    --with-http_ssl_module \
    --with-http_mp4_module \
    --with-http_v2_module \
    --without-http_upstream_zone_module \
    --add-module=${ROOT_PWD}/nginx-rtmp-module

# 如果在 x86 主机上为 RK3568 交叉编译
# ./configure \
#     --with-cc=aarch64-linux-gnu-gcc \
#     --with-cpp=aarch64-linux-gnu-cpp \
#     --prefix=third_lib/nginx-1.24.0/install \
#     ...其他参数...

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