# CMake最低版本要求
cmake_minimum_required(VERSION 3.5)

# 项目名称和语言
project(video-capture C CXX)

# 设置C标准为C11，C++标准为C++20
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 是否编译测试（默认关闭）
option(BUILD_TESTS "Build test programs" ON)

# 是否启用Rockchip MPP模块（默认关闭）
option(ENABLE_MPP "Enable Rockchip MPP module" OFF)

# ============================================================
# FFmpeg 配置（仅 Windows 平台）
# ============================================================
if(WIN32)
    set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-8.1)
    set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/lib)

    set(FFMPEG_LIBRARIES
        ${FFMPEG_LIB_DIR}/avcodec.lib
        ${FFMPEG_LIB_DIR}/avformat.lib
        ${FFMPEG_LIB_DIR}/avutil.lib
        ${FFMPEG_LIB_DIR}/swscale.lib
        ${FFMPEG_LIB_DIR}/swresample.lib
        ${FFMPEG_LIB_DIR}/avfilter.lib
        ${FFMPEG_LIB_DIR}/avdevice.lib
    )
endif()

# ============================================================
# ASIO (header-only)
# ============================================================
set(ASIO_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/asio-1.36.0)
add_library(asio INTERFACE IMPORTED)
target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

# ============================================================
# spdlog (header-only)
# ============================================================
set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/spdlog-1.17.0/include)
add_library(spdlog INTERFACE IMPORTED)
target_include_directories(spdlog INTERFACE ${SPDLOG_INCLUDE_DIR})

# 加载工具链和 SDK 配置（仅Linux平台）
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(${CMAKE_SOURCE_DIR}/cmake/toolchain_config.cmake)
else()
    # Windows平台需要单独定义第三方库路径
    set(THIRD_PARTY_INCLUDE_DIRS
        ${SPDLOG_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/pcap/Include
    )
endif()

# 包含头文件路径
# 项目内部头文件（include/ 目录）
include_directories(${CMAKE_SOURCE_DIR}/include)

if(ENABLE_MPP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include_directories(${MPP_INCLUDE_DIRS})
endif()
if(WIN32)
    include_directories(${FFMPEG_INCLUDE_DIR})
endif()
include_directories(${THIRD_PARTY_INCLUDE_DIRS})

# 链接库路径（仅Linux平台）
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    link_directories(${LIBRARY_DIRS})
endif()

# 收集所有源文件
file(GLOB_RECURSE SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

# 定义可执行文件
add_executable(video-capture ${SOURCES})

# 链接 asio 和 spdlog（header-only 库）
target_link_libraries(video-capture asio spdlog)

# 根据平台链接不同的库
if(WIN32)
    # Windows平台链接库
    target_link_libraries(video-capture
        ws2_32      # Windows Socket
        ${FFMPEG_LIBRARIES}
    )
    
    # 将 FFmpeg DLL 复制到输出目录
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    add_custom_command(TARGET video-capture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_DLLS}
            $<TARGET_FILE_DIR:video-capture>
        COMMENT "Copying FFmpeg DLLs to output directory"
    )
    
    message(STATUS "Platform: Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux平台链接库
    target_link_libraries(video-capture
        pthread
        dl
    )
    
    # 如果启用了MPP模块，链接Rockchip MPP库
    if(ENABLE_MPP)
        target_link_libraries(video-capture
            rockchip_mpp
        )
        message(STATUS "MPP Module: Enabled")
    else()
        message(STATUS "MPP Module: Disabled (set ENABLE_MPP=ON to enable)")
    endif()
    
    message(STATUS "Platform: Linux")
endif()

# 编译选项 - 根据平台添加调试信息和警告
if(MSVC)
    # MSVC编译器选项
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /utf-8")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /utf-8")
    set(CMAKE_C_FLAGS_DEBUG "/Zi /Od")
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od")
    set(CMAKE_C_FLAGS_RELEASE "/O2")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2")
else()
    # GCC/Clang编译器选项
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_C_FLAGS_DEBUG "-g -O0")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    set(CMAKE_C_FLAGS_RELEASE "-O2")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
endif()

# 是否编译测试
if(BUILD_TESTS)
    message(STATUS "Building tests: ON")
    
    # spdlog 测试
    add_executable(test_spdlog ${CMAKE_SOURCE_DIR}/test/test_spdlog.cpp)
    
    # 链接 spdlog（header-only 库）
    target_link_libraries(test_spdlog spdlog)
    
    # 根据平台链接不同的库
    if(WIN32)
        target_link_libraries(test_spdlog ws2_32)
    else()
        target_link_libraries(test_spdlog pthread)
    endif()
    
    message(STATUS "Test targets: test_spdlog")
else()
    message(STATUS "Building tests: OFF (set BUILD_TESTS=ON to enable)")
endif()

# 打印配置信息（便于调试CMake配置）
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "===========================================")