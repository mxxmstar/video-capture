#pragma once
// Selected by the platform CMake target, implementations stay in src/common.
#if defined(EMBEDDED_PROCESS_POSIX)
#include "common/process/rockchip/process.h"
#else
#include "common/process/generic/process.h"
#endif
