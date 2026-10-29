#!/usr/bin/env bash
set -euo pipefail
platform_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$platform_dir/../.." && pwd)
archive=${1:?Usage: build-spdlog.sh /path/to/gabime-spdlog-v1.17.0.tar.gz}
source_dir="$project_dir/build-deps/spdlog-src"
build_dir="$project_dir/build-deps/spdlog-arm64"
mkdir -p "$source_dir"
tar xzf "$archive" -C "$source_dir" --strip-components=1
cmake -S "$source_dir" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$platform_dir/toolchains/nanopc-t4-gcc12.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_INSTALL_PREFIX="$platform_dir/deps/aarch64" -DCMAKE_INSTALL_LIBDIR=lib \
  -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_FMT_EXTERNAL=OFF \
  -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF \
  -DSPDLOG_BUILD_BENCH=OFF -DSPDLOG_INSTALL=ON
cmake --build "$build_dir" -j4
cmake --install "$build_dir"
install -Dm644 "$source_dir/LICENSE" "$platform_dir/deps/aarch64/share/licenses/spdlog/LICENSE"
