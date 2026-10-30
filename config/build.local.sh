# Copy to build.local.sh and edit for this build host.
# This is trusted Bash code sourced by scripts/build-menu.sh.
# PROJECT_ROOT is the absolute source directory; quote each -D argument.
# Paths below are EXAMPLES. Use libraries built for the selected target ABI.
BUILD_ROOT="$PROJECT_ROOT/build/menu"
JOBS=${JOBS:-4}

# Shared feature flags, not target-specific SDK paths.
COMMON_CMAKE_ARGS=(
    "-DBUILD_RENDER=OFF"
    "-DBUILD_TESTS=ON"
)

# Native Linux: Boost (system/filesystem/process), spdlog and FFmpeg 8.x.
# This profile uses preinstalled dependencies without invoking vcpkg.
GENERIC_CMAKE_ARGS=(
    "-DEMBEDDED_USE_VCPKG=OFF"
    "-DCMAKE_TOOLCHAIN_FILE="
    "-DCMAKE_PREFIX_PATH=/home/ub22/vcpkg_env/vcpkg_installed/x64-linux"
    "-DFFMPEG_ROOT=/home/ub22/vcpkg_env/ffmpeg-n8.1-latest-linux64-gpl-shared-8.1"
    "-DMEDIA_FFMPEG_BACKEND=ON"
)

# Current board profile: NanoPC-T4 / RK3399, Linux ARM64, GCC12 + device ABI.
# Paths are independent of the native Linux SDK above.
ROCKCHIP_CMAKE_ARGS=(
    "-DCMAKE_TOOLCHAIN_FILE=$PROJECT_ROOT/platform/rockchip/toolchains/nanopc-t4-gcc12.cmake"
    "-DNANOPC_SYSROOT=$PROJECT_ROOT/platform/rockchip/sysroots/nanopc-t4"
    "-DROCKCHIP_DEPS_ROOT=$PROJECT_ROOT/platform/rockchip/deps/aarch64"
    "-DMEDIA_FFMPEG_BACKEND=OFF"
    "-DROCKCHIP_WITH_MPP=ON"
    "-DROCKCHIP_WITH_RGA=OFF"
)
# For another ARM64 board, provide a matching toolchain and sysroot;
# NANOPC_SYSROOT is specific to the bundled NanoPC-T4 toolchain.
# A different CPU/vendor backend also needs CMake/source platform support.
