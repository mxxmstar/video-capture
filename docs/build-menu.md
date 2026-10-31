# 脚本菜单与依赖位置配置


## 快速使用

从工程根目录执行：

**依据当前主机环境修改 build.local.sh 中的依赖路径**

```bash
bash scripts/build-menu.sh
```

当前 ub22 主机的 build.local.sh 已按 build-x86-modules 的已有依赖路径配置，无需重新复制示例覆盖它。

菜单：1/2 为 generic Release/Debug；3/4 为 rockchip Release/Debug；c1-c4 清理对应构建；h 显示菜单；q 或输入 EOF 退出。选择一次构建后退出，失败返回非零状态。

```bash
# 查看菜单或预览命令，不执行配置/编译
bash scripts/build-menu.sh --list
bash scripts/build-menu.sh --platform generic --type Debug --dry-run

# 无交互编译、仅配置、清理
JOBS=4 bash scripts/build-menu.sh --platform generic --type Release
bash scripts/build-menu.sh --platform rockchip --type Debug --action configure
bash scripts/build-menu.sh --platform generic --type Release --action clean
```

构建目录为 `build/menu/<platform>/<Release|Debug>`，程序位于对应目录的 `bin/`。clean 调用 CMake clean 目标，保留缓存，不递归删除目录。更换编译器、SDK、sysroot 或生成器时，在配置中改用新的 BUILD_ROOT，避免旧缓存残留；普通 clean 不会清除 CMakeCache。

可通过 `--config` 或环境变量 `EMBEDDED_BUILD_CONFIG` 指定配置文件，命令行优先。配置是可信 Bash 代码，只加载自己维护的文件。数组中每个 CMake 参数单独加引号，支持带空格的路径；多个 CMAKE_PREFIX_PATH 使用分号分隔。

| 配置项 | 含义 |
| --- | --- |
| BUILD_ROOT | 本机构建输出根目录，平台和类型在其下自动分开 |
| COMMON_CMAKE_ARGS | 两个平台共用的功能选项 |
| GENERIC_CMAKE_ARGS | 本机 Linux SDK 参数，不能指向 ARM64 库 |
| ROCKCHIP_CMAKE_ARGS | ARM64 工具链、sysroot、MPP/RGA 等参数 |
| CMAKE_PREFIX_PATH | Boost/spdlog 等 CMake 包安装前缀，含 include/lib/share 等 |
| FFMPEG_ROOT | 通用平台 FFmpeg 8.x SDK，含 include/ 与 lib/ |
| CMAKE_TOOLCHAIN_FILE | 目标工具链，在 CMake project() 前生效 |
| NANOPC_SYSROOT | 随工程 NanoPC-T4 工具链使用的设备开发环境 |
| ROCKCHIP_DEPS_ROOT | ARM64 私有 SDK 前缀 |

当前 generic 使用 `EMBEDDED_USE_VCPKG=OFF`，直接查找已安装 SDK，和原 build-x86-modules 配置一致。若改用 vcpkg，开启此选项并将工具链参数改成实际 vcpkg.cmake；同时设置 VCPKG_INSTALLED_DIR/VCPKG_TARGET_TRIPLET 等参数，使用新的 BUILD_ROOT。

## 在另一台主机或目标板上编译

1. 在新的 Linux 编译主机准备 Bash、CMake >= 3.21、构建工具和支持 C++20 的编译器。菜单用于 Linux/Bash；Windows 本机构建可继续直接使用 CMake。
2. 复制配置示例到 build.local.sh，并安装匹配目标架构的 SDK。配置只记录位置，不会下载、安装或复制依赖库。
3. generic 当前需要 Boost system/filesystem/process；启用媒体 FFmpeg 后端时需要 spdlog 与匹配的新 FFmpeg SDK。模板关闭渲染，若开启还需 OpenGL/EGL、GLFW、glad 等依赖。
4. rockchip 模板针对 NanoPC-T4 / RK3399：交叉编译器 GCC12，sysroot 提供设备 GCC10 标准库及 glibc 2.31 ABI。MPP 默认开启、RGA 关闭，按设备准备头文件和库。参见 [Rockchip 说明](../platform/rockchip/README.md)。
5. 先使用 --dry-run 检查路径，再 --action configure，最后构建。ARM64 程序应在目标设备验证，不在 x86 主机直接运行。

Rockchip 当前 `MEDIA_FFMPEG_FULL=OFF`，编译媒体 contracts/core 及其已有测试，不提供完整 FFmpeg 媒体功能；现有 CMake 明确拒绝 rockchip + MEDIA_FFMPEG_FULL=ON。仅准备 ARM64 FFmpeg SDK 不会自动实现该后端。

换一台编译主机通常只需调整本机配置；换目标板时必须匹配 CPU、ABI、sysroot、驱动和工具链。当前 NanoPC-T4 工具链固定了设备 ABI，不能通用于所有 ARM64 设备；新芯片平台可能还需要扩展 cmake/Platform.cmake 和平台实现。

本配置描述编译期依赖位置，不是运行库部署清单。动态库仍需按设备运行环境打包和验证；不要用主机 x86 库或不匹配的 glibc 替换设备库。

