# 多平台构建

根 CMakeLists.txt 仅选择平台、加载功能选项和添加源码/测试模块；第三方库由每个子平台独立决定。

```text
cmake/Platform.cmake                  # 平台选择和 setup 入口
cmake/BuildOptions.cmake              # 功能开关、输出目录
cmake/FFmpeg.cmake                    # 通用 SDK 查找辅助函数
platform/generic/setup.cmake          # 宿主机 vcpkg 初始化
platform/generic/CMakeLists.txt       # Boost / spdlog / FFmpeg 依赖
src/common/process/generic/process.cpp   # 现有 Boost.Process 实现
src/common/process/rockchip/process.cpp   # 无 Boost 的 POSIX 进程实现
platform/rockchip/setup.cmake         # ARM64 工具链选择
platform/rockchip/CMakeLists.txt      # MPP / RGA / spdlog / FFmpeg，无 Boost
src/common/CMakeLists.txt             # 公共目标与所选进程后端
modules/media/CMakeLists.txt              # 媒体源码
```

`EMBEDDED_PLATFORM=generic|rockchip`，默认 generic；不支持的值明确报错。

通用 Linux / Windows：

```bash
cmake -S . -B build-generic-platform \
  -DEMBEDDED_PLATFORM=generic -DBUILD_RENDER=OFF \
  -DFFMPEG_ROOT=/path/to/ffmpeg-8.x
cmake --build build-generic-platform -j4
```

通用平台默认保留 vcpkg，可通过 EMBEDDED_USE_VCPKG=OFF 和 CMAKE_PREFIX_PATH 指定已有依赖。Rockchip 不使用此选项，不加载宿主机 vcpkg。

Rockchip：见 [平台说明](../platform/rockchip/README.md)。旧 boards 目录已迁移成 platform，sysroot 和依赖一起迁移。旧构建目录保存绝对路径，切换后使用新构建目录，不要重用旧 CMakeCache。

功能项：BUILD_MEDIA、BUILD_RENDER、BUILD_V4L2_CAPTURE、BUILD_TESTS、BUILD_MEDIA_TESTS、MEDIA_RAW_FRAME_BUFFER。关闭 BUILD_MEDIA 可独立验证设备进程模块，无需 FFmpeg/spdlog；BUILD_RENDER 依赖 BUILD_MEDIA。测试选择当前平台的进程后端，媒体测试不再引用 Boost.Asio。

MediaStreamSession 采用无参构造，调用代码由 `make_shared<MediaStreamSession>(io)` 改为 `make_shared<MediaStreamSession>()`；内部线程模型不变。

不包含新硬编解码实现、安装打包或设备系统库升级。
