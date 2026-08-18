# ============================================================================
# Makefile — RK3568 视觉流水线构建脚本
# ============================================================================
#
# 构建方式：
#   1) aarch64（板端原生）：make —— 链接板端 FFmpeg/RGA/MPP + third_lib 的 RKNN，
#      生成可执行文件 output/rk3568_vision
#   2) x86_64 交叉编译：    make CROSS_COMPILE=aarch64-linux-gnu-
#      —— 在 x86 开发机上编出 aarch64 可执行文件，直接拷到板端运行
#      （aarch64 的 FFmpeg/RGA/MPP 头文件+库已入库在 third_lib/aarch64-sysroot/）
#   3) x86_64 纯编译检查：  make check —— 只编译到 .o 做语法检查
#
# 构建类型：
#   make            # release：-O2 优化
#   make debug      # debug：-DVISION_DEBUG -g -O0
#   make clean      # 清理 build/ 与 output/
# ============================================================================

CROSS_COMPILE ?=
SYSROOT ?= third_lib/aarch64-sysroot

ARCH := $(shell uname -m)

BUILD ?= release

CXX := $(CROSS_COMPILE)g++
BASE_FLAGS := -std=c++17 -Wall -Wextra -Iinclude -Ithird_lib/librknn_api/include
ifeq ($(BUILD),debug)
    CXXFLAGS := $(BASE_FLAGS) -DVISION_DEBUG -g -O0
    TARGET   := output/rk3568_vision_debug
else
    CXXFLAGS := $(BASE_FLAGS) -O2
    TARGET   := output/rk3568_vision
endif

# ---------------------------------------------------------------------------
# 依赖库：原生用 pkg-config；交叉编译用 SYSROOT 里的 aarch64 头文件/库
# ---------------------------------------------------------------------------
ifeq ($(CROSS_COMPILE),)
    FFMPEG := $(shell pkg-config --cflags --libs libavcodec libavformat libavutil libswscale 2>/dev/null)
    RGA    := $(shell pkg-config --libs librga 2>/dev/null)
    MPP    := $(shell pkg-config --libs rockchip_mpp 2>/dev/null || echo -lrockchip_mpp)
    CROSS_LDFLAGS :=
else
    CXXFLAGS += -I$(SYSROOT)/usr/include
    FFMPEG := -L$(SYSROOT)/usr/lib -lavcodec -lavformat -lavutil -lswscale
    RGA    := -lrga
    MPP    := -lrockchip_mpp
    # 交叉链接时 FFmpeg 共享库引用的第三方编解码库（libtheora/libvorbis 等）
    # 不在 sysroot 里，用 --allow-shlib-undefined 放行，运行时由板端动态库解析。
    CROSS_LDFLAGS := -Wl,--allow-shlib-undefined
endif
LDFLAGS := $(FFMPEG) -lpthread $(RGA) $(MPP) $(CROSS_LDFLAGS)

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

# ---------------------------------------------------------------------------
# 是否生成可执行文件：板端原生 aarch64，或交叉编译
# ---------------------------------------------------------------------------
ifeq ($(CROSS_COMPILE),)
    ifeq ($(ARCH),aarch64)
        LINK_TARGET := $(TARGET)
    endif
else
    LINK_TARGET := $(TARGET)
endif

ifneq ($(LINK_TARGET),)
    # 链接 third_lib 的 librknnrt.so 2.3.2，并通过 rpath 让运行时优先加载 third_lib 版本。
    LDFLAGS    += third_lib/librknn_api/aarch64/librknnrt.so \
                  -Wl,-rpath,'$$ORIGIN/../third_lib/librknn_api/aarch64'
    ALL_TARGET := $(LINK_TARGET)
else
    ALL_TARGET := check
endif

.PHONY: all debug check clean

all: $(ALL_TARGET)

debug:
	$(MAKE) BUILD=debug

$(TARGET): $(OBJS)
	@mkdir -p output
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "==> 构建完成: $@ ($(BUILD), $(if $(CROSS_COMPILE),交叉编译 aarch64,原生 $(ARCH)))"

check: $(OBJS)
	@echo "==> x86 编译检查通过（生成可执行文件需板端 make 或交叉编译）"

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build output
