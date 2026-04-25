cmake_minimum_required(VERSION 3.10)

# ============================================================================
# 项目声明 (交叉编译设置必须在此行之前完成)
# ============================================================================
set(PROJECT_NAME app)
project(${PROJECT_NAME} C CXX)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/share/bin")
message(STATUS "=================================================================================")


# ============================================================================
# 平台检测与编译模式选择 
# ============================================================================
# 定义控制开关：默认 OFF。除非显式传递 -DCOMPILE_X86=ON，否则默认交叉编译 RK
option(COMPILE_X86 "明确指定编译 x86 版本 (默认 OFF -> 交叉编译 RK3568)" OFF)

# 自动检测宿主机 CPU 架构
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" HOST_ARCH)
if(HOST_ARCH MATCHES "aarch64|arm64")
    # 场景：当前直接在 RK3568 板子上执行 cmake
    set(USE_CROSS_COMPILE OFF)
    message(STATUS "Host: aarch64 | Mode: Native Compilation (自动识别板端环境)")
else()
    # 场景：在 x86 Ubuntu 上执行
    if(COMPILE_X86)
        set(USE_CROSS_COMPILE OFF)
        message(STATUS "Host: x86_64 | Mode: Native x86 (显式指定 -DCOMPILE_X86=ON)")
    else()
        set(USE_CROSS_COMPILE ON)
        message(STATUS "Host: x86_64 | Mode: Cross Compile for RK3568 (默认行为)")
    endif()
endif()

# ================= 交叉编译环境配置 =================
# 注意：所有交叉编译相关变量必须在 project() 声明前设置，否则 CMake 会忽略它们！
if(USE_CROSS_COMPILE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)

    set(TOOLCHAIN_PATH "/usr/local/arm64/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu")
    set(CROSS_COMPILE "aarch64-linux-gnu")

    set(CMAKE_C_COMPILER   "${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}-g++")

    # 关键：彻底隔离 x86 宿主机库，强制在 sysroot 中搜索头文件和库
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()

# ============================================================================
# 3. 编译选项与标准设置
# ============================================================================
# 统一编译标志 (替代原 C_FLAGS/CXX_FLAGS 混写，更规范)
add_compile_options(-Wall -Wextra -pthread -g)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # 禁用 GNU 扩展，严格 C99
set(CMAKE_CXX_STANDARD 11)

# 调试/日志宏开关
option(DEBUG_ENABLE "Enable debug output" OFF)
option(LOG_ENABLE "Enable log to file" ON)

if(DEBUG_ENABLE)
    add_definitions(-DDEBUG_ENABLE)
endif()
if(LOG_ENABLE)
    add_definitions(-DLOG_ENABLE)
endif()

# ============================================================================
# 源文件列表
# ============================================================================
file(GLOB SRC_C src/*.c)
add_executable(${PROJECT_NAME}
    main.cc
    ${SRC_C}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ============================================================================
# OpenCV 集成 (自动适配交叉/原生模式)
# ============================================================================
if(USE_CROSS_COMPILE)
    # 【交叉编译模式】使用手动指定的 RK 版 OpenCV 路径
    set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/third_lib/opencv/lib/cmake/opencv4")
    find_package(OpenCV REQUIRED) # 默认使用 CONFIG 模式（寻找 OpenCVConfig.cmake
    message(STATUS "OpenCV version: ${OpenCV_VERSION}")
    message(STATUS "OpenCV include path: ${OpenCV_INCLUDE_DIRS}")
    include_directories(${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} ${OpenCV_LIBS})

    # set(OPENCV_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_lib/opencv")
    # if(NOT EXISTS "${OPENCV_PATH}")
    #    message(FATAL_ERROR "交叉编译 OpenCV 路径未找到: ${OPENCV_PATH}\n请先执行 build_opencv.sh")
    # endif()
    
    # target_include_directories(${PROJECT_NAME} PRIVATE "${OPENCV_PATH}/include/opencv4")
    # set(OPENCV_LIB_DIR "${OPENCV_PATH}/lib")
    
    # # 架构校验：防止误拷入 x86 版本的 OpenCV
    # execute_process(
    #    COMMAND file "${OPENCV_LIB_DIR}/libopencv_core.so"
    #    OUTPUT_VARIABLE OPENCV_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE
    # )
    # message(STATUS "OpenCV core arch check: ${OPENCV_ARCH}")
    # if(OPENCV_ARCH MATCHES "x86-64")
    #    message(FATAL_ERROR "架构校验失败！检测到 x86 版本的 OpenCV，请重新交叉编译")
    # endif()
    
    # # 显式链接 RK 版 OpenCV 核心库
    # target_link_libraries(${PROJECT_NAME} PRIVATE
    #    "${OPENCV_LIB_DIR}/libopencv_core.so"
    #    "${OPENCV_LIB_DIR}/libopencv_imgproc.so"
    #    "${OPENCV_LIB_DIR}/libopencv_imgcodecs.so"
    #    "${OPENCV_LIB_DIR}/libopencv_videoio.so"
    #    "${OPENCV_LIB_DIR}/libopencv_highgui.so"
    #    pthread
    # )
else()
    # 【原生编译模式】(x86 Ubuntu 或 RK 本机) 使用系统 find_package
    find_package(OpenCV REQUIRED)
    message(STATUS "Found System OpenCV: ${OpenCV_VERSION} | ${OpenCV_INCLUDE_DIRS}")
    
    target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS} pthread)
endif()

if(USE_CROSS_COMPILE)
    # 交叉编译：设置 RPATH，使编译出的 app 能在 RK 上自动找到同级 lib 目录
    # $ORIGIN 表示可执行文件所在目录
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "$ORIGIN/../../third_lib/opencv/lib"
        INSTALL_RPATH "$ORIGIN/../../third_lib/opencv/lib"
    )
endif()

# ============================================================================
# 输出路径与 RPATH (动态库搜索路径)
# ============================================================================
# set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

# ============================================================================
# 安装规则
# ============================================================================
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "${OUTPUT_DIR}"
)

# ============================================================================
# 8. 配置日志打印
# ============================================================================
message(STATUS "=================================================================================")
message(STATUS "=> Compile Mode : ${USE_CROSS_COMPILE} (ON=Cross RK, OFF=Native)")
message(STATUS "=> DEBUG_ENABLE : ${DEBUG_ENABLE}")
message(STATUS "=> LOG_ENABLE   : ${LOG_ENABLE}")
if(USE_CROSS_COMPILE)
    message(STATUS "=> SYSROOT      : ${SYSROOT}")
    message(STATUS "=> OpenCV Path  : ${OPENCV_LIB_DIR}")
else()
    message(STATUS "=> OpenCV Ver   : ${OpenCV_VERSION}")
endif()
message(STATUS "=================================================================================")