# ============================================================================
# Makefile — RK3568 视觉流水线构建脚本
# ============================================================================
#
# 平台自动判断（uname -m）：
#   aarch64（RK3568 板端）：链接真实 RKNN 运行时 librknnrt.so，生成可执行文件
#   x86_64（开发机）：      只编译到 .o 做语法检查（不链接，因为 RKNN 需板端 NPU）
#
# 用法：
#   make          # aarch64 构建可执行文件 / x86 编译检查
#   make clean    # 清理
# ============================================================================

ARCH := $(shell uname -m)

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -Ithird_lib/librknn_api/include
FFMPEG   := $(shell pkg-config --cflags --libs libavcodec libavformat libavutil libswscale)
LDFLAGS  := $(FFMPEG) -lpthread

SRCS   := $(wildcard src/*.cpp)
OBJS   := $(patsubst src/%.cpp,build/%.o,$(SRCS))
TARGET := build/rk3568_vision

ifeq ($(ARCH),aarch64)
    LDFLAGS    += third_lib/librknn_api/aarch64/librknnrt.so
    ALL_TARGET := $(TARGET)
else
    ALL_TARGET := check
endif

.PHONY: all check clean

all: $(ALL_TARGET)

# 完整构建（aarch64 板端）
$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "==> 构建完成: $@"

# x86 编译检查：只编译到 .o，不链接（真实 RKNN 推理需在 aarch64 板端）
check: $(OBJS)
	@echo "==> x86 编译检查通过（真实推理需在 RK3568 板端链接/运行）"

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build
