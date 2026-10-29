set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

get_filename_component(TOOLCHAIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(NANOPC_SYSROOT "${TOOLCHAIN_ROOT}/sysroots/nanopc-t4" CACHE PATH
    "Ubuntu 20.04 ARM64 sysroot exported from NanoPC-T4")
if(NOT EXISTS "${NANOPC_SYSROOT}/usr/include/features.h")
    message(FATAL_ERROR "Missing NanoPC sysroot: ${NANOPC_SYSROOT}")
endif()
set(CMAKE_SYSROOT "${NANOPC_SYSROOT}")
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-10)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-10)

# Use the device's C++10 headers and libraries, not Jammy's ARM64 runtime.
# --sysroot alone does not override GCC's own standard-library search paths.
set(TARGET_CXX_HEADERS "${CMAKE_SYSROOT}/usr/include/c++/10")
set(TARGET_CXX_ARCH_HEADERS "${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu/c++/10")
set(CMAKE_C_FLAGS_INIT "-B${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -nostdinc++ -isystem ${TARGET_CXX_HEADERS} -isystem ${TARGET_CXX_ARCH_HEADERS} -isystem ${TARGET_CXX_HEADERS}/backward")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${CMAKE_SYSROOT}/usr/lib/gcc/aarch64-linux-gnu/10 -L${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES NANOPC_SYSROOT)
