# CMake最低版本要求
cmake_minimum_required(VERSION 3.0)

# 项目名称和语言
project(rk_mpp_demo C CXX)

# 设置C标准为C11，C++标准为C++20
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 是否编译测试（默认关闭）
option(BUILD_TESTS "Build test programs" OFF)

# 加载工具链和 SDK 配置
include(${CMAKE_SOURCE_DIR}/cmake/toolchain_config.cmake)

# 包含头文件路径
include_directories(${MPP_INCLUDE_DIRS})
include_directories(${THIRD_PARTY_INCLUDE_DIRS})

# 链接库路径
link_directories(${LIBRARY_DIRS})

# 定义可执行文件
add_executable(mpp_demo ${CMAKE_SOURCE_DIR}/src/main.cpp)

# 链接MPP相关库
# rockchip_mpp - MPP核心库
# pthread - 线程库
# dl - 动态加载库
target_link_libraries(mpp_demo
    rockchip_mpp
    pthread
    dl
)

# 编译选项 - 添加调试信息和警告
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# 是否编译测试
if(BUILD_TESTS)
    message(STATUS "Building tests: ON")
    
    # spdlog 测试
    add_executable(test_spdlog ${CMAKE_SOURCE_DIR}/test/test_spdlog.cpp)
    target_link_libraries(test_spdlog pthread)
    
    message(STATUS "Test targets: test_spdlog")
else()
    message(STATUS "Building tests: OFF (set BUILD_TESTS=ON to enable)")
endif()

# 打印配置信息（便于调试CMake配置）
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "===========================================")