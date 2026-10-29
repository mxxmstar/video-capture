# 设置目标平台
set(EMBEDDED_PLATFORM "generic" CACHE STRING "Target platform")
set_property(CACHE EMBEDDED_PLATFORM PROPERTY STRINGS generic rockchip)
if(NOT EMBEDDED_PLATFORM MATCHES "^(generic|rockchip)$")
    message(FATAL_ERROR "Unsupported EMBEDDED_PLATFORM='${EMBEDDED_PLATFORM}'; choose generic or rockchip")
endif()
# 设置平台目录
set(EMBEDDED_PLATFORM_DIR "${CMAKE_CURRENT_LIST_DIR}/../platform/${EMBEDDED_PLATFORM}")
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{CMAKE_TOOLCHAIN_FILE}" CACHE FILEPATH "Explicit environment toolchain")
endif()
include("${EMBEDDED_PLATFORM_DIR}/setup.cmake")
message(STATUS "Embedded platform: ${EMBEDDED_PLATFORM}")
