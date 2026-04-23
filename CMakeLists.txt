cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME main)
project(${PROJECT_NAME})

# 设置C标准
set(OUTPUT_DIR output)
# 不使用任何C编译器标志,使用C++11标准进行编译
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
# 强制使用 C99 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # 禁用 GNU 扩展，严格使用 C99

# 可选：设置调试模式（默认开启DEBUG）
# 可以通过 cmake -DV4L2_DEBUG_ENABLE=OFF 来关闭
option(V4L2_DEBUG_ENABLE "Enable V4L2 debug output" OFF)
option(V4L2_LOG_ENABLE "Enable V4L2 log to file" ON)

# 根据选项添加宏定义
if(V4L2_DEBUG_ENABLE)
    # add_definitions(-DV4L2_DEBUG_ENABLE)
endif()

if(V4L2_LOG_ENABLE)
    add_definitions(-DV4L2_LOG_ENABLE)
endif()

# 源文件列表（包含所有依赖）
# 源文件列表
set(SOURCES
    main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)
# 包含头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${SOURCES})


find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME} ${OpenCV_LIBS})

# 链接库
target_link_libraries(${PROJECT_NAME}
    pthread
)

# 可选：设置输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}/
)


# 安装可执行文件
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/${OUTPUT_DIR}/
)

# 打印配置信息
message(STATUS "=> [V4L2_DEBUG_ENABLE]: ${V4L2_DEBUG_ENABLE}")
message(STATUS "=> [V4L2_LOG_ENABLE]: ${V4L2_LOG_ENABLE}")
message(STATUS "=> [Source files]: ${SOURCES}")