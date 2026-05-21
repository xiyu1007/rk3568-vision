# ============================================================================
# CMakeLists.txt — RK3568 Vision Pipeline 构建配置
# ============================================================================
# 
# **构建目标**：生成可执行文件 rk3568_vision
#
# **编译语言**：C11 + C++17（混合编译）
#   - C 模块（*.c）：pipeline, v4l2, rknn（C封装）, encoder, rtmp, logger, config, etc.
#   - C++ 模块（*.cpp）：detector, rknn_context（RAII）, bridge, display
#
# **平台适配**：
#   - RK3568 aarch64：链接真实 librknnrt.so + librga.so + OpenCV NEON 优化版
#   - x86_64 开发机：定义 X86_DEBUG 宏，RKNN 使用 stub（代码可编译但推理不执行）
#
# **第三方依赖**：
#   - OpenCV：图像处理、颜色转换、本地显示（NEON SIMD 优化）
#   - RKNN（aarch64 only）：NPU 推理运行时库
#   - RGA（aarch64 only）：Rockchip 硬件图形加速（NV12→BGR 零拷贝转换，待启用）
#   - FFmpeg：libx264 软件编码 + RTMP 推流
#   - pthreads：多线程支持
#   - DRM（aarch64 optional）：零拷贝直接显示到屏幕
# ============================================================================

cmake_minimum_required(VERSION 3.16)           # 最小 CMake 版本要求
set(PROJECT_NAME rk3568_vision)                 # 项目名称
project(${PROJECT_NAME}
    VERSION 3.0.0                               # 版本号
    DESCRIPTION "RK3568 Edge AI Vision Pipeline (C core + C++ OpenCV)"
    LANGUAGES C CXX                             # 同时启用 C 和 C++ 编译器
)

set(OPENCV_PATH "/third_lib/opencv")
set(RKNN_PATH "/third_lib/librknn_api")
set(RGA_PATH "/third_lib/rga")

# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------

# AddressSanitizer（地址消毒器）：检测内存越界、use-after-free 等
# 仅在调试时启用（显著影响性能）
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

# ---------------------------------------------------------------------------
# 平台检测
# ---------------------------------------------------------------------------
# CMAKE_HOST_SYSTEM_PROCESSOR 返回运行 CMake 的主机 CPU 架构
# 交叉编译时，这通常是编译主机（x86_64），不是目标（aarch64）
# 但在 RK3568 上直接编译时，CMAKE_HOST_SYSTEM_PROCESSOR 就是 aarch64
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" HOST_ARCH)


if(HOST_ARCH MATCHES "aarch64|arm64")
    set(IS_RK3568 ON)                            # 目标平台：RK3568
    add_definitions(-DRK3568_NATIVE)             # 定义 RK3568_NATIVE 宏
    message(STATUS "Platform: aarch64 native (RK3568)")
else()
    set(IS_RK3568 OFF)                           # 开发平台：x86_64
    add_definitions(-DX86_DEBUG)                 # 定义 X86_DEBUG 宏（RKNN stub）
    message(STATUS "Platform: x86_64 (RKNN stub)")
endif()

# ---------------------------------------------------------------------------
# C/C++ 标准
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)                         # C11 标准
set(CMAKE_CXX_STANDARD 17)                       # C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)              # 强制要求 C++17（不可降级）
set(CMAKE_CXX_EXTENSIONS OFF)                    # 禁用编译器扩展（GNU 扩展等）
set(CMAKE_POSITION_INDEPENDENT_CODE ON)          # 生成位置无关代码（-fPIC）

# ---------------------------------------------------------------------------
# 编译警告和优化
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wno-unused-parameter)  # 启用大部分警告

# 默认构建类型为 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG)            # O2 优化 + 禁用断言
else()
    add_compile_options(-O0 -g3)                 # 无优化 + 完整调试符号
endif()

# AddressSanitizer 编译/链接选项
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

file(GLOB SRC_C src/*.c)
file(GLOB SRC_CC src/*.cc)
file(GLOB SRC_CPP src/*.cpp)
add_executable(${PROJECT_NAME}
    ${SRC_C}
    ${SRC_CC}
    ${SRC_CPP}
)

# 头文件搜索路径
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # 项目头文件目录
    ${CMAKE_CURRENT_SOURCE_DIR}            # 项目根目录（用于 include "include/xxx.h"）
)

# Debug 构建额外宏定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:RK3568_DEBUG>
)

# ---------------------------------------------------------------------------
# 第三方库依赖
# ---------------------------------------------------------------------------

# --- OpenCV ---
message(STATUS "=========================================================")
if(EXISTS "${CMAKE_SOURCE_DIR}/${OPENCV_PATH}/lib/cmake/opencv4") 
    message(STATUS "Using custom OpenCV")

    set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/${OPENCV_PATH}/lib/cmake/opencv4")
    find_package(OpenCV REQUIRED) # 默认使用 CONFIG 模式（寻找 OpenCVConfig.cmake
    include_directories(${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS}
    )
    message(STATUS "OpenCV version: ${OpenCV_VERSION}")
    message(STATUS "OpenCV include path: ${OpenCV_INCLUDE_DIRS}")
else()
    message(STATUS "Using system OpenCV")
    # # 【原生编译模式】(x86 Ubuntu 或 RK 本机) 使用系统 find_package
    find_package(OpenCV REQUIRED)
    # find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui)
    target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})

    message(STATUS "OpenCV version: ${OpenCV_VERSION}")
    message(STATUS "OpenCV include path: ${OpenCV_INCLUDE_DIRS}")
endif()

# --- RKNN（仅 aarch64 平台） ---
message(STATUS "=========================================================")
# RK3568 的 NPU 推理运行时库
# 优先使用系统安装的 librknnrt.so，否则使用 third_lib 中的版本
# if(IS_RK3568)
# 手动指定 RKNN 库路径
set(RKNN_LIB_DIR "${CMAKE_SOURCE_DIR}/${RKNN_PATH}" CACHE PATH "RKNN lib path")
target_include_directories(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/aarch64/librknnrt.so")
message(STATUS "RKNN: manual path ${RKNN_LIB_DIR}")
# endif()

# --- RGA（Rockchip Graphics API，仅 aarch64） ---
# RGA 是 Rockchip 的 2D 硬件加速引擎
# 可用于 NV12→RGB 零拷贝转换（比 CPU NEON 更快）
# 如果未找到，降级使用 CPU 转换（bridge.cpp 中的 OpenCV cvtColor）
message(STATUS "=========================================================")
find_library(RGA_LIBRARY NAMES rga librga
    PATHS ${CMAKE_SOURCE_DIR}/${RGA_PATH}/lib /usr/lib /usr/lib/aarch64-linux-gnu)
find_path(RGA_INCLUDE_DIR NAMES rga.h im2d.h 
    PATHS ${CMAKE_SOURCE_DIR}/${RGA_PATH}/include /usr/include/rga)
if(RGA_LIBRARY AND RGA_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${RGA_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${RGA_LIBRARY})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_RGA)
    message(STATUS "RGA: ${RGA_LIBRARY}")
else()
    message(STATUS "RGA: not found, fallback to CPU resize")
endif()

# --- FFmpeg ---
# 使用 pkg-config 查找 FFmpeg 组件
# libavcodec：编解码器（libx264）
# libavformat：封装格式（FLV、RTMP）
# libavutil：工具函数
# libswscale：像素格式转换（NV12→YUV420P）
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
    message(STATUS "FFmpeg: found via pkg-config")
endif()

# --- Threads（POSIX 线程） ---
# -lpthread，用于创建采集/推理/编码/显示四个工作线程
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# --- DRM（Direct Rendering Manager，仅 aarch64） ---
# DRM 用于零拷贝直接显示到屏幕（绕过 X11/Wayland）
# 当前版本使用 OpenCV highgui 显示，DRM 作为日后硬件直显的备选方案
if(IS_RK3568)
    find_library(DRM_LIBRARY NAMES drm)
    if(DRM_LIBRARY)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${DRM_LIBRARY})
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DRM)
        message(STATUS "DRM: found")
    endif()
endif()

# ---------------------------------------------------------------------------
# RPATH（运行时动态库搜索路径）
# ---------------------------------------------------------------------------
# 设置可执行文件内部的 RPATH，使其在 RK3568 上能自动找到 third_lib 中的 .so 文件
# $ORIGIN 表示可执行文件所在目录
# BUILD_RPATH：构建时使用的搜索路径
# INSTALL_RPATH：安装后使用的搜索路径
if(IS_RK3568)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH   "$ORIGIN/../${OPENCV_PATH}/lib:$ORIGIN/../${RKNN_PATH}/aarch64"
        INSTALL_RPATH "$ORIGIN/../${OPENCV_PATH}/lib:$ORIGIN/../${RKNN_PATH}/aarch64"
    )
endif()

# 安装规则：将可执行文件安装到 bin 目录
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# 构建摘要（输出关键配置信息）
# ---------------------------------------------------------------------------
message(STATUS "========================================")
message(STATUS "  Project:     ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build Type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Std:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "  CC:          ${CMAKE_C_COMPILER}")
message(STATUS "  CXX:         ${CMAKE_CXX_COMPILER}")
message(STATUS "========================================")
