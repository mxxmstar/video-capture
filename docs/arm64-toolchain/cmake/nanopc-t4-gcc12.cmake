# Modern C++ compiler with the target's existing C++10 / glibc 2.31 ABI.
include("${CMAKE_CURRENT_LIST_DIR}/nanopc-t4-gcc10.cmake")
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-12)
