cmake_minimum_required(VERSION 3.16)
set(PROJECT_NAME rk3568_vision)
project(${PROJECT_NAME}
    VERSION 3.0.0
    DESCRIPTION "RK3568 Edge AI Vision Pipeline (C core + C++ OpenCV)"
    LANGUAGES C CXX
)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" HOST_ARCH)

if(HOST_ARCH MATCHES "aarch64|arm64")
    set(IS_RK3568 ON)
    add_definitions(-DRK3568_NATIVE)
    message(STATUS "Platform: aarch64 native (RK3568)")
else()
    set(IS_RK3568 OFF)
    add_definitions(-DX86_DEBUG)
    message(STATUS "Platform: x86_64 (RKNN stub)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG)
else()
    add_compile_options(-O0 -g3)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

set(SOURCES
    src/main.c
    src/config.c
    src/logger.c
    src/fps.c
    src/sig.c
    src/v4l2.c
    src/perf.c
    src/monitor.c
    src/encoder.c
    src/rtmp.c
    src/rknn.c
    src/rknn_context.cpp
    src/pipeline.c
    src/bridge.cpp
    src/detector.cpp
    src/display.cpp
)

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
)

# ============================================================================
# Third-party dependencies
# ============================================================================

# --- OpenCV ---
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui)
target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
message(STATUS "OpenCV: ${OpenCV_VERSION}")

# --- RKNN (aarch64 only) ---
if(IS_RK3568)
    find_package(RKNN QUIET)
    if(RKNN_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE RKNN::RKNN)
        message(STATUS "RKNN: ${RKNN_VERSION}")
    else()
        set(RKNN_LIB_DIR "${CMAKE_SOURCE_DIR}/third_lib/librknn_api" CACHE PATH "RKNN lib path")
        target_include_directories(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/include")
        target_link_libraries(${PROJECT_NAME} PRIVATE "${RKNN_LIB_DIR}/aarch64/librknnrt.so")
        message(STATUS "RKNN: manual path ${RKNN_LIB_DIR}")
    endif()
endif()

# --- RGA (Rockchip Graphics API) ---
if(IS_RK3568)
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
if(IS_RK3568)
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
if(IS_RK3568)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH   "$ORIGIN/../third_lib/opencv/lib:$ORIGIN/../third_lib/librknn_api/aarch64"
        INSTALL_RPATH "$ORIGIN/../third_lib/opencv/lib:$ORIGIN/../third_lib/librknn_api/aarch64"
    )
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

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
