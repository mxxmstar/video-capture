# 使用目标的 GCC 10 / glibc 2.31 ABI.
include("${CMAKE_CURRENT_LIST_DIR}/nanopc-t4-gcc10.cmake")

# 指定编译器为 GCC 12 交叉编译器
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-12)
