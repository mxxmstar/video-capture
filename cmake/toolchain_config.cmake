# ============================================================
# RV1126 工具链和 SDK 路径配置
# ============================================================
# 此文件集中管理交叉编译工具链和 SDK 路径
# 修改 SDK 路径时只需修改此文件
# ============================================================

# ============================================================
# 交叉编译工具链配置
# ============================================================

# RV1126使用ARM32架构，需要指定交叉编译工具链
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Buildroot 工具链路径
set(TOOLCHAIN_PATH /opt/sdk/RV1126/buildroot/output/rockchip_rv1126_rv1109/host/bin)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PATH}/arm-linux-gnueabihf-g++)

# ============================================================
# SDK 路径配置
# ============================================================

# MPP SDK 根目录
set(MPP_SDK_PATH /opt/sdk/RV1126/external/mpp)

# Buildroot 目标文件系统路径
set(BUILDROOT_TARGET_PATH /opt/sdk/RV1126/buildroot/output/rockchip_rv1126_rv1109/target)
set(BUILDROOT_OEM_PATH /opt/sdk/RV1126/buildroot/output/rockchip_rv1126_rv1109/oem)

# ============================================================
# 头文件路径
# ============================================================

# MPP 头文件目录
set(MPP_INCLUDE_DIRS
    ${MPP_SDK_PATH}/inc              # MPP公共头文件目录 (rk_mpi.h, rk_type.h, mpp_frame.h等)
)

# 第三方库头文件
set(THIRD_PARTY_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/third_party/spdlog-1.17.0/include
)

# ============================================================
# 库文件路径
# ============================================================

# 链接库搜索路径
set(LIBRARY_DIRS
    ${BUILDROOT_OEM_PATH}/usr/lib    # MPP库文件目录
    ${BUILDROOT_TARGET_PATH}/usr/lib # 系统库目录
    ${BUILDROOT_TARGET_PATH}/lib     # 系统库目录
)

# ============================================================
# 打印配置信息（调试用）
# ============================================================

message(STATUS "===========================================")
message(STATUS "Toolchain & SDK Configuration")
message(STATUS "===========================================")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "MPP SDK Path: ${MPP_SDK_PATH}")
message(STATUS "Buildroot Target: ${BUILDROOT_TARGET_PATH}")
message(STATUS "===========================================")