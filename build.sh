#!/bin/bash

# RV1126 MPP Demo 编译脚本
# 使用方法：
#   交互模式（直接运行）：
#     ./build.sh                  # 显示交互式菜单
#
#   命令行模式：
#     ./build.sh build debug      # Debug编译
#     ./build.sh build release    # Release编译
#     ./build.sh build debug test # Debug编译，包含测试
#     ./build.sh build release test # Release编译，包含测试
#     ./build.sh clean            # 清理所有
#     ./build.sh clean main       # 只清理主程序
#     ./build.sh clean test       # 只清理测试程序

# 构建函数
do_build() {
    local BUILD_TYPE=$1
    local BUILD_TEST=$2
    
    echo "=========================================="
    echo "--- Build Script ---"
    echo "=========================================="
    echo "Build Type: $BUILD_TYPE"
    echo "Build Tests: $BUILD_TEST"
    echo "=========================================="
    
    # 创建build目录
    mkdir -p build
    cd build
    
    # 设置CMake选项
    local CMAKE_TEST_FLAG=""
    if [ "$BUILD_TEST" = "ON" ]; then
        CMAKE_TEST_FLAG="-DBUILD_TESTS=ON"
    fi
    
    # 配置CMake
    echo ""
    echo "Running CMake configuration..."
    cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE $CMAKE_TEST_FLAG
    
    if [ $? -ne 0 ]; then
        echo "[ERROR] CMake configuration failed!"
        exit 1
    fi
    
    # 编译
    echo ""
    echo "Building..."
    make -j$(nproc)
    
    if [ $? -ne 0 ]; then
        echo "[ERROR] Build failed!"
        exit 1
    fi
    
    echo ""
    echo "=========================================="
    echo "Build successful!"
    echo "Output:"
    echo "  - build/mpp_demo"
    if [ "$BUILD_TEST" = "ON" ]; then
        echo "  - build/test_spdlog"
    fi
    echo "=========================================="
    
    # 显示生成的文件信息
    echo ""
    echo "Binary info:"
    if [ -f mpp_demo ]; then
        file mpp_demo
        ls -lh mpp_demo
    fi
    if [ -f test_spdlog ]; then
        echo ""
        file test_spdlog
        ls -lh test_spdlog
    fi
}

# 清理函数
do_clean() {
    local CLEAN_TARGET=${1:-all}
    CLEAN_TARGET=$(echo "$CLEAN_TARGET" | tr '[:lower:]' '[:upper:]')
    
    echo "=========================================="
    echo "RV1126 MPP Demo Clean Script"
    echo "=========================================="
    
    # 确保 build 目录存在
    if [ ! -d "build" ]; then
        echo "Build directory does not exist, nothing to clean."
        return 0
    fi
    
    case "$CLEAN_TARGET" in
        ALL)
            echo "Cleaning all (removing build directory)..."
            rm -rf build
            echo "Clean completed."
            ;;
        MAIN)
            echo "Cleaning main program..."
            cd build
            # 删除主程序相关文件
            rm -f mpp_demo
            rm -rf CMakeFiles/mpp_demo.dir
            # 清理 CMake 缓存（如果需要重新配置）
            rm -f CMakeCache.txt cmake_install.cmake Makefile
            rm -f CMakeFiles/*.log CMakeFiles/*.cache
            rm -rf CMakeFiles/3.16.3
            rm -f CMakeFiles/Makefile2 CMakeFiles/Makefile.cmake
            rm -f CMakeFiles/TargetDirectories.txt CMakeFiles/cmake.check_cache
            rm -f CMakeFiles/progress.marks CMakeFiles/CMakeDirectoryInformation.cmake
            echo "Main program cleaned."
            ;;
        TEST)
            echo "Cleaning test programs..."
            cd build
            # 删除测试程序相关文件
            rm -f test_spdlog
            rm -rf CMakeFiles/test_spdlog.dir
            rm -rf logs
            echo "Test programs cleaned."
            ;;
        *)
            echo "[ERROR] Unknown clean target: $1"
            echo "Available targets: all, main, test"
            return 1
            ;;
    esac
    
    echo "=========================================="
}

# 如果没有参数，显示交互式菜单
if [ $# -eq 0 ]; then
    while true; do
        echo ""
        echo "=========================================="
        echo "  RV1126 MPP Demo Build Menu"
        echo "=========================================="
        echo "  1. Build Debug"
        echo "  2. Build Release"
        echo "  3. Clean and Build Debug"
        echo "  4. Clean and Build Release"
        echo "  5. Build Test (Release)"
        echo "  6. Clean All"
        echo "  0. Exit"
        echo "=========================================="
        echo -n "Please select [0-6]: "
        read choice
        
        case $choice in
            1)
                echo ""
                do_build "Debug" "OFF"
                break
                ;;
            2)
                echo ""
                do_build "Release" "OFF"
                break
                ;;
            3)
                echo ""
                do_clean "all"
                echo ""
                do_build "Debug" "OFF"
                break
                ;;
            4)
                echo ""
                do_clean "all"
                echo ""
                do_build "Release" "OFF"
                break
                ;;
            5)
                echo ""
                do_build "Release" "ON"
                break
                ;;
            6)
                echo ""
                do_clean "all"
                break
                ;;
            0)
                echo "Exit."
                exit 0
                ;;
            *)
                echo ""
                echo "[ERROR] Invalid option, please select 0-6"
                echo ""
                read -p "Press Enter to continue..."
                ;;
        esac
    done
    exit 0
fi

# 命令行模式
ACTION=${1:-build}
ACTION=$(echo "$ACTION" | tr '[:upper:]' '[:lower:]')

case "$ACTION" in
    build)
        BUILD_TYPE=${2:-Release}
        BUILD_TYPE=$(echo "$BUILD_TYPE" | tr '[:lower:]' '[:upper:]')
        BUILD_TEST=${3:-off}
        BUILD_TEST=$(echo "$BUILD_TEST" | tr '[:lower:]' '[:upper:]')
        
        if [ "$BUILD_TEST" = "ON" ] || [ "$BUILD_TEST" = "YES" ] || [ "$BUILD_TEST" = "TEST" ] || [ "$BUILD_TEST" = "TRUE" ]; then
            BUILD_TEST="ON"
        else
            BUILD_TEST="OFF"
        fi
        
        do_build "$BUILD_TYPE" "$BUILD_TEST"
        ;;
    clean)
        CLEAN_TARGET=${2:-all}
        do_clean "$CLEAN_TARGET"
        ;;
    *)
        echo "[ERROR] Unknown action: $ACTION"
        echo "Usage:"
        echo "  ./build.sh                    # Interactive menu"
        echo "  ./build.sh build [debug|release] [test]  # Build"
        echo "  ./build.sh clean [all|main|test]         # Clean"
        exit 1
        ;;
esac