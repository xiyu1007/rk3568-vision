# ============================================================================
# Makefile — RK3568 视觉流水线构建脚本
# ============================================================================
#
# 平台自动判断（uname -m）：
#   aarch64（RK3568 板端）：链接真实 RKNN 运行时 librknnrt.so，生成可执行文件
#   x86_64（开发机）：      只编译到 .o 做语法检查（不链接，因为 RKNN 需板端 NPU）
#
# 构建类型：
#   make            # release：-O2 优化
#   make debug      # debug：-DVISION_DEBUG -g -O0，开启性能分析/队列深度/详细日志
#   make clean      # 清理 build/ 与 output/
#
# 目录约定：
#   build/   编译中间产物（.o）
#   output/  生成的可执行文件与录制输出
#   log/     运行日志
#   conf/    配置
#   third_lib/  三方依赖（rknn_api + mediamtx，fetch_deps.sh 拉取，不入库）
# ============================================================================

ARCH := $(shell uname -m)

# 构建类型：release（默认）/ debug。
BUILD ?= release

CXX := g++
BASE_FLAGS := -std=c++17 -Wall -Wextra -Iinclude -Ithird_lib/librknn_api/include
ifeq ($(BUILD),debug)
    CXXFLAGS := $(BASE_FLAGS) -DVISION_DEBUG -g -O0
    TARGET   := output/rk3568_vision_debug
else
    CXXFLAGS := $(BASE_FLAGS) -O2
    TARGET   := output/rk3568_vision
endif

FFMPEG  := $(shell pkg-config --cflags --libs libavcodec libavformat libavutil libswscale)
RGA     := $(shell pkg-config --libs librga)
LDFLAGS := $(FFMPEG) -lpthread $(RGA)

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

ifeq ($(ARCH),aarch64)
    # 链接 third_lib 的 librknnrt.so 2.3.2（模型需 2.x 运行时），
    # 并通过 rpath 让运行时优先加载 third_lib 的版本，不覆盖系统 /lib 的 2.1.0。
    LDFLAGS    += third_lib/librknn_api/aarch64/librknnrt.so \
                  -Wl,-rpath,'$$ORIGIN/../third_lib/librknn_api/aarch64'
    ALL_TARGET := $(TARGET)
else
    ALL_TARGET := check
endif

.PHONY: all debug check clean

all: $(ALL_TARGET)

# debug 构建（与 release 输出不同文件名，便于并存对比）。
debug:
	$(MAKE) BUILD=debug

# 完整构建（aarch64 板端）。
$(TARGET): $(OBJS)
	@mkdir -p output
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "==> 构建完成: $@ ($(BUILD))"

# x86 编译检查：只编译到 .o，不链接（真实 RKNN 推理需在 aarch64 板端）。
check: $(OBJS)
	@echo "==> x86 编译检查通过（真实推理需在 RK3568 板端链接/运行）"

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build output
