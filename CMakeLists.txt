cmake_minimum_required(VERSION 3.16)
set(PROJECT_NAME rk3568_vision)
project(${PROJECT_NAME}
    VERSION 2.0.0
    DESCRIPTION "RK3568 Edge AI Vision Pipeline"
    LANGUAGES CXX C
)

# ============================================================================
# 编译选项
# ============================================================================
option(BUILD_TESTS           "Build unit tests"              OFF)
option(BUILD_EXAMPLES        "Build example applications"    OFF)
option(ENABLE_ASAN           "Enable AddressSanitizer"       OFF)
# GStreamer removed — not used on RK3568 target
option(CROSS_COMPILE_RK      "Cross-compile for RK3568"      OFF)

# ============================================================================
# 交叉编译检测
# ============================================================================
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" HOST_ARCH)
if(HOST_ARCH MATCHES "aarch64|arm64")
    set(CROSS_COMPILE_RK OFF)
    add_definitions(-DRK3568_NATIVE)
    message(STATUS "Host: aarch64 - Board native compile mode")
elseif(CROSS_COMPILE_RK)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain_rk3568.cmake"
        CACHE FILEPATH "RK3568 cross-compile toolchain" FORCE)
    message(STATUS "Host: x86_64 - Cross compile for RK3568")
else()
    add_definitions(-DX86_DEBUG)
    message(STATUS "Host: x86_64 - x86 debug mode")
endif()

# ============================================================================
# C++ Standard: C++17
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# Compiler flags
# ============================================================================
add_compile_options(-Wall -Wextra)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG -ffast-math -funroll-loops)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3 -DRK3568_DEBUG)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# ============================================================================
# Source files
# ============================================================================
set(SOURCES
    src/v4l2_capture.cpp
    src/logger.cpp
    src/config.cpp
    src/sig_handler.c
    src/fps.c
    src/perf.cpp
    src/monitor.cpp
    src/buffer_pool.cpp
    src/rknn_context.cpp
    src/preprocess.cpp
    src/postprocess.cpp
    src/detector.cpp
    src/pipeline.cpp
    src/ffmpeg_encoder.cpp
    src/rtmp_pusher.cpp
    src/display.cpp
    src/osd.cpp
    src/main.cpp
)

# GStreamer encoder removed — not used on RK3568

# ============================================================================
# Executable target
# ============================================================================
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:RK3568_DEBUG>
    $<$<BOOL:${CROSS_COMPILE_RK}>:CROSS_COMPILE>
)

# ============================================================================
# Third-party dependencies
# ============================================================================

# --- OpenCV ---
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui)
target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
message(STATUS "OpenCV: ${OpenCV_VERSION}")

# --- RKNN (cross-compile or board only) ---
if(CROSS_COMPILE_RK OR HOST_ARCH MATCHES "aarch64|arm64")
    find_package(RKNN QUIET)
    if(RKNN_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE RKNN::RKNN)
        message(STATUS "RKNN: ${RKNN_VERSION}")
    else()
        set(RKNN_LIB_DIR "${CMAKE_SOURCE_DIR}/third_lib/rknn" CACHE PATH "RKNN lib path")
        target_include_directories(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/include")
        target_link_libraries(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/lib/librknnrt.so")
        message(STATUS "RKNN: manual path ${RKNN_LIB_DIR}")
    endif()
endif()

# --- RGA (Rockchip Graphics API) ---
if(CROSS_COMPILE_RK OR HOST_ARCH MATCHES "aarch64|arm64")
    find_library(RGA_LIBRARY NAMES rga librga
        PATHS /usr/lib /usr/lib/aarch64-linux-gnu
        ${CMAKE_SOURCE_DIR}/third_lib/rga/lib)
    find_path(RGA_INCLUDE_DIR NAMES rga.h im2d.h
        PATHS /usr/include/rga
        ${CMAKE_SOURCE_DIR}/third_lib/rga/include)
    if(RGA_LIBRARY AND RGA_INCLUDE_DIR)
        target_include_directories(${PROJECT_NAME} PRIVATE ${RGA_INCLUDE_DIR})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${RGA_LIBRARY})
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_RGA)
        message(STATUS "RGA: ${RGA_LIBRARY}")
    else()
        message(STATUS "RGA: not found, fallback to CPU resize")
    endif()
endif()

# --- FFmpeg ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
    message(STATUS "FFmpeg: found via pkg-config")
endif()

# GStreamer removed — not used on RK3568 target

# --- Threads ---
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# --- DRM (optional, zero-copy display) ---
if(CROSS_COMPILE_RK OR HOST_ARCH MATCHES "aarch64|arm64")
    find_library(DRM_LIBRARY NAMES drm)
    if(DRM_LIBRARY)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${DRM_LIBRARY})
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DRM)
        message(STATUS "DRM: found")
    endif()
endif()

# ============================================================================
# RPATH for dynamic library search
# ============================================================================
if(CROSS_COMPILE_RK)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH   "$ORIGIN/../third_lib/opencv/lib:$ORIGIN/../third_lib/rknn/lib"
        INSTALL_RPATH "$ORIGIN/../third_lib/opencv/lib:$ORIGIN/../third_lib/rknn/lib"
    )
endif()

# ============================================================================
# Install rules
# ============================================================================
set(OUTPUT_DIR "share/bin")
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${OUTPUT_DIR}")
install(DIRECTORY config/ DESTINATION "share/config" FILES_MATCHING PATTERN "*.yaml")
install(DIRECTORY model/ DESTINATION "share/model" FILES_MATCHING PATTERN "*.rknn" PATTERN "*.txt")

# ============================================================================
# Build summary
# ============================================================================
message(STATUS "========================================")
message(STATUS "  Project:     ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build Type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Std:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "  CC:          ${CMAKE_C_COMPILER}")
message(STATUS "  CXX:         ${CMAKE_CXX_COMPILER}")
message(STATUS "========================================")
