# ============================================================================
# CMakeLists.txt — rk3568-vision 构建配置
# ============================================================================
#
# 构建目标：可执行文件 rk3568_vision
#
# 平台适配（双模式）：
#   - aarch64（RK3568）：定义 VISION_RK3568，链接真实 RKNN 运行时库，
#                       编码走 h264_rkmpp 硬件编码。
#   - x86_64（开发机）：  定义 VISION_X86，RKNN 使用 mock 推理，
#                       编码走 libx264 软件编码。
#
# 依赖：
#   - FFmpeg（libavcodec/libavformat/libavutil/libswscale）
#   - RKNN 运行时（仅 aarch64）
#   - pthread（多线程）
# ============================================================================

cmake_minimum_required(VERSION 3.16)

set(PROJECT_NAME rk3568_vision)
project(${PROJECT_NAME}
    VERSION 4.0.0
    DESCRIPTION "RK3568 Edge AI Vision Pipeline (pure C++)"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------
option(ENABLE_ASAN "Enable AddressSanitizer (debug only)" OFF)

# C++17 标准（强制）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release；Debug 时开完整符号。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# 平台检测
# ---------------------------------------------------------------------------
# CMAKE_SYSTEM_PROCESSOR 在交叉编译时为 aarch64，本机为 x86_64。
# 这里统一按【目标处理器】判断，交叉编译工具链会设置 CMAKE_SYSTEM_PROCESSOR=aarch64。
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" TARGET_ARCH)

if(TARGET_ARCH MATCHES "aarch64|arm64")
    set(IS_RK3568 ON)
    add_compile_definitions(VISION_RK3568)
    message(STATUS "Platform: aarch64 (RK3568) — real RKNN + h264_rkmpp")
else()
    set(IS_RK3568 OFF)
    add_compile_definitions(VISION_X86)
    message(STATUS "Platform: ${TARGET_ARCH} — mock RKNN + libx264")
endif()

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SOURCES src/*.cpp)
add_executable(${PROJECT_NAME} ${SOURCES})

# 头文件搜索路径。
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    third_lib/json
    third_lib/rknn/include
)

# 编译警告。
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# Debug / Release 优化级别。
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -O2)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -O0 -g3)
endif()

# AddressSanitizer。
if(ENABLE_ASAN)
    target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# 线程
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
    message(STATUS "FFmpeg: found via pkg-config")
else()
    message(FATAL_ERROR "pkg-config not found, cannot locate FFmpeg")
endif()

# ---------------------------------------------------------------------------
# RKNN（仅 aarch64）
# ---------------------------------------------------------------------------
if(IS_RK3568)
    set(RKNN_LIB "${CMAKE_SOURCE_DIR}/third_lib/rknn/aarch64/librknnrt.so")
    if(NOT EXISTS "${RKNN_LIB}")
        message(FATAL_ERROR "librknnrt.so not found at ${RKNN_LIB}, run scripts/fetch_deps.sh first")
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${RKNN_LIB}")
    message(STATUS "RKNN: linked ${RKNN_LIB}")
endif()

# ---------------------------------------------------------------------------
# RPATH（运行期动态库搜索路径，仅 aarch64）
# ---------------------------------------------------------------------------
if(IS_RK3568)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH   "$ORIGIN/../third_lib/rknn/aarch64"
        INSTALL_RPATH "$ORIGIN/../third_lib/rknn/aarch64"
    )
endif()

# ---------------------------------------------------------------------------
# 构建摘要
# ---------------------------------------------------------------------------
message(STATUS "================================================")
message(STATUS "  Project    : ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "================================================")
