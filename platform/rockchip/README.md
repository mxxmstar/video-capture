# Rockchip / NanoPC-T4 构建与依赖交叉编译

本文命令除标注“设备端”外，均在 Ubuntu 虚拟机执行。维护日期：2026-10-29。

## 1. 环境与目录

| 项目 | 当前环境 |
| --- | --- |
| 编译主机 | ub22@192.168.117.131，Ubuntu 22.04 x86_64 |
| 工程 | /home/ub22/mxxmstar/embedded-device |
| 目标设备 | pi@192.168.66.6，NanoPC-T4 / RK3399，Ubuntu 20.04 AArch64 |
| 设备镜像 | rk3399-XYZ-ubuntu-focal-lxqt-desktop-4.19-arm64-YYYYMMDD.img.gz |
| 编译器 | aarch64-linux-gnu-gcc-12 / g++-12 |
| 目标 ABI | 设备 glibc 2.31、GCC 10 C++ 标准库 |

所有工程构建命令从工程根目录执行：

```bash
cd /home/ub22/mxxmstar/embedded-device
```

```text
platform/rockchip/
├── CMakeLists.txt                   # 本平台依赖、后端选择
├── setup.cmake                      # project() 前选择工具链
├── toolchains/
│   ├── nanopc-t4-gcc12.cmake         # 默认工具链
│   └── nanopc-t4-gcc10.cmake         # 目标 ABI 配置基底
├── export-sysroot.py                # 在设备导出基础开发环境
├── prepare-sysroot.py               # 修复导出后的绝对符号链接
├── build-spdlog.sh                  # spdlog 重建脚本
├── sysroots/nanopc-t4/              # 设备基础环境
└── deps/aarch64/                    # 工程的 ARM64 第三方依赖
    ├── include/spdlog/
    ├── lib/libspdlog.a
    ├── lib/cmake/spdlog/
    ├── share/licenses/spdlog/
    └── ffmpeg/                     # 尚待准备
        ├── include/
        └── lib/
```

sysroot 提供设备基础头文件、启动文件和系统库；deps 放工程单独编译的第三方 SDK。不要将虚拟机 x86_64 库复制进 ARM64 目录。

平台目录不放 C++ 业务实现。公共入口为 `include/common/process/process.h`，平台头文件位于 `include/common/process/generic/`、`include/common/process/rockchip/`；对应实现位于 `src/common/process/generic/`、`src/common/process/rockchip/`。各子平台 CMake 独立管理依赖，业务目标通过 `Embedded::Platform` 获得平台宏及链接依赖。

## 2. 准备编译工具与 sysroot

新虚拟机安装工具：

```bash
sudo apt-get update
sudo apt-get install -y g++-12-aarch64-linux-gnu cmake make python3 tar pkg-config
```

默认 GCC 12 工具链包含 GCC 10 工具链文件中的 ABI 配置，再覆盖编译器为 GCC 12；使用默认工具链不要求同时安装 GCC 10 交叉编译器。
工具链会显式使用 sysroot 中 GCC 10 标准库头文件、库和设备启动文件，避免混入 Ubuntu 22.04 的目标运行库。

首次导出（目标设备需已有 `/usr/lib/gcc/aarch64-linux-gnu/10` 等开发文件）：

```bash
scp platform/rockchip/export-sysroot.py pi@192.168.66.6:/tmp/export-sysroot.py
ssh pi@192.168.66.6 \
  'python3 /tmp/export-sysroot.py /tmp/nanopc-t4-sysroot.tar.gz'
scp pi@192.168.66.6:/tmp/nanopc-t4-sysroot.tar.gz /tmp/nanopc-t4-sysroot.tar.gz

# 仅用于尚未配置的 sysroot；已有环境不要直接覆盖混装。
mkdir -p platform/rockchip/sysroots/nanopc-t4
tar xzf /tmp/nanopc-t4-sysroot.tar.gz \
  -C platform/rockchip/sysroots/nanopc-t4
python3 platform/rockchip/prepare-sysroot.py \
  platform/rockchip/sysroots/nanopc-t4
```

`prepare-sysroot.py` 将 `/lib/...` 等设备绝对符号链接转成 sysroot 内的相对链接，并在缺失时建立 `lib -> usr/lib`。两个 lib 入口不是两份库，不要删除。脚本只修改导出的 sysroot，不修改设备系统。

检查：

```bash
ls -ld platform/rockchip/sysroots/nanopc-t4/{lib,usr/lib}
ls platform/rockchip/sysroots/nanopc-t4/usr/include/features.h
ls platform/rockchip/sysroots/nanopc-t4/usr/include/c++/10/span
ls -l platform/rockchip/sysroots/nanopc-t4/usr/lib/aarch64-linux-gnu/{libc.so.6,libc_nonshared.a,libpthread.so,ld-linux-aarch64.so.1}
```

## 编译依赖

### 1. spdlog：已完成交叉编译

已验证版本 **1.17.0**，构建为静态库，使用内置 fmt，不需要额外构建 fmt。源码包复用虚拟机 vcpkg 的下载缓存，编译过程不使用其 x64 库。

```bash
sha256sum /home/ub22/vcpkg/downloads/gabime-spdlog-v1.17.0.tar.gz
# d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744

bash platform/rockchip/build-spdlog.sh \
  /home/ub22/vcpkg/downloads/gabime-spdlog-v1.17.0.tar.gz
```

新环境需自行准备相同版本源码包，参数可替换为实际路径。**脚本解压到 `build-deps/spdlog-src`，构建到 `build-deps/spdlog-arm64`，安装到 `platform/rockchip/deps/aarch64`。**切换源码版本、sysroot 或工具链时使用干净的源码/构建目录，避免残留文件。

脚本对应的核心配置：

```bash
cmake -S build-deps/spdlog-src -B build-deps/spdlog-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/platform/rockchip/toolchains/nanopc-t4-gcc12.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_INSTALL_PREFIX="$PWD/platform/rockchip/deps/aarch64" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_FMT_EXTERNAL=OFF \
  -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF \
  -DSPDLOG_BUILD_BENCH=OFF -DSPDLOG_INSTALL=ON
cmake --build build-deps/spdlog-arm64 -j4
cmake --install build-deps/spdlog-arm64
```

脚本还会安装 spdlog LICENSE。fmt 的许可证随内置头文件保留。

验证安装与架构：

```bash
ls platform/rockchip/deps/aarch64/lib/libspdlog.a
ls platform/rockchip/deps/aarch64/lib/cmake/spdlog/spdlogConfig.cmake
aarch64-linux-gnu-readelf -h platform/rockchip/deps/aarch64/lib/libspdlog.a \
  | grep Machine
```

现有工程的 `logger.h` 已通过 ARM64 编译并在设备运行，输出 `ARM64 spdlog 1.17.0 works`。静态库的日志实现已链接进程序，不需要部署 libspdlog.so 或 libfmt.so。

### 2. FFmpeg：设备已有，但与当前工程不兼容

已确认设备安装 **4.2.7-1rockchip-r6**，包括运行库、开发头文件和以下七个组件：avcodec、avformat、avutil、avdevice、avfilter、swscale、swresample。

检查命令：

```bash
ssh pi@192.168.66.6 'ffmpeg -version'
ssh pi@192.168.66.6 \
  'pkg-config --modversion libavcodec libavformat libavutil libavdevice libavfilter libswscale libswresample'
```

工程使用 `AVChannelLayout`、`ch_layout`、`av_channel_layout_*` 等新 API，设备 FFmpeg 4.2 不支持。现有 SDK 配置按 FFmpeg 8.x 准备；不能用新版头文件链接设备旧库。设备虽然安装了开发包，目前精简 sysroot 导出脚本也没有打包 FFmpeg 动态库。

**当前状态：FFmpeg 8.x ARM64 SDK 尚未编译和验证。** 建议作为工程私有 SDK 安装到 `deps/aarch64/ffmpeg`，不替换设备系统版本。

下面是后续构建的基础命令模板，尚未在本次工作中执行。先准备并记录所选 FFmpeg 8.x 源码版本和 SHA-256，将未配置的源码解压到 `build-deps/ffmpeg-src`。FFmpeg 使用 configure，不会自动读取 CMake 工具链，所以必须显式传递 sysroot 和 ABI 路径：

```bash
PROJECT_ROOT="$PWD"
RK_SYSROOT="$PROJECT_ROOT/platform/rockchip/sysroots/nanopc-t4"
FFMPEG_PREFIX="$PROJECT_ROOT/platform/rockchip/deps/aarch64/ffmpeg"
mkdir -p build-deps/ffmpeg-arm64
cd build-deps/ffmpeg-arm64

../ffmpeg-src/configure \
  --prefix="$FFMPEG_PREFIX" \
  --enable-cross-compile --target-os=linux --arch=aarch64 \
  --cross-prefix=aarch64-linux-gnu- \
  --cc=aarch64-linux-gnu-gcc-12 --cxx=aarch64-linux-gnu-g++-12 \
  --sysroot="$RK_SYSROOT" \
  --extra-cflags="-B$RK_SYSROOT/usr/lib/aarch64-linux-gnu/" \
  --extra-cxxflags="-B$RK_SYSROOT/usr/lib/aarch64-linux-gnu/ -nostdinc++ -isystem $RK_SYSROOT/usr/include/c++/10 -isystem $RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/10 -isystem $RK_SYSROOT/usr/include/c++/10/backward" \
  --extra-ldflags="-L$RK_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/10 -L$RK_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/lib/aarch64-linux-gnu" \
  --enable-shared --disable-static --enable-pic \
  --disable-autodetect --disable-programs --disable-doc
make -j4
make install
cd "$PROJECT_ROOT"
```

这是基础 SDK 配置，不包含 libx264/libx265、TLS 外部库或 MPP 集成；不能据此保证 HTTPS、软件 H.264 编码或 Rockchip 硬编解码可用。若现有测试指定 `libx264`，需另行交叉编译 x264 并在 FFmpeg 启用，或按最终编码方案调整测试。依赖功能、授权条件、版本和实际运行需在正式构建时逐项确认。

安装后先用 `file` / `readelf` 确认库为 AArch64，再配置整个工程并在设备验证；不要在 x86 主机上对 ARM64 程序运行普通 ldd。

## 5. 工程配置和验证

每套平台、工具链使用独立构建目录。当前 spdlog 已准备好，完整媒体构建仍会在缺少 FFmpeg SDK 处停止。

```bash
cmake -S . -B build-rk-platform \
  -DEMBEDDED_PLATFORM=rockchip -DCMAKE_BUILD_TYPE=Release
cmake --build build-rk-platform -j4
```

默认路径：`NANOPC_SYSROOT=platform/rockchip/sysroots/nanopc-t4`、`ROCKCHIP_DEPS_ROOT=platform/rockchip/deps/aarch64`、`FFMPEG_ROOT=platform/rockchip/deps/aarch64/ffmpeg`。这些都是 CMake PATH 缓存项，可显式传绝对路径覆盖。

仅构建基础模块，无需 spdlog 和 FFmpeg：

```bash
cmake -S . -B build-rk-core \
  -DEMBEDDED_PLATFORM=rockchip -DBUILD_MEDIA=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-rk-core -j4
```

RK 不查找、不链接 Boost。进程管理用 glibc 的 posix_spawnp/waitid/waitpid/kill 与标准库。MPP 默认开启、RGA 默认关闭，分别通过 `ROCKCHIP_WITH_MPP` / `ROCKCHIP_WITH_RGA` 控制；它们从 deps 或设备 sysroot 查找，不代表已经实现硬编解码。渲染默认关闭，开启需额外准备 ARM64 OpenGL/EGL、GLFW、glad。

## 6. 常见问题、Git 与部署边界

| 现象 | 检查与处理 |
| --- | --- |
| 找不到 libc.so.6 / libc_nonshared.a / ld-linux | 检查 sysroot 文件、lib 入口，以及真实链接命令是否带正确 --sysroot；不要只加 -L |
| pthread 识别失败 | 先执行 prepare-sysroot.py 修复绝对链接；检查最终 FindThreads 结果。glibc 2.31 下 CMAKE_HAVE_LIBC_PTHREAD 失败、随后 -lpthread 成功属于正常探测 |
| 修复后仍重复旧错误 | 使用新的构建目录，旧 CMakeError.log 会保留失败探测记录，以终端最后的 CMake Error 为准 |
| 找不到 spdlogConfig.cmake | 重建 spdlog，检查 deps/aarch64/lib/cmake/spdlog，并确认 ROCKCHIP_DEPS_ROOT |
| AVChannelLayout/ch_layout 不存在 | 错用了 FFmpeg 4.x 头文件；头文件和库必须来自同一套新版 ARM64 SDK |
| GLIBC / GLIBCXX 版本超过设备支持 | 检查是否混入虚拟机目标运行库，用 readelf --version-info 检查最终 ELF |
| deps 中库没有显示在 Git | platform/rockchip/.gitignore 忽略 /deps/ 和 /sysroots/，空目录也不会被 Git 跟踪 |

当前只版本管理平台配置和脚本。需要依赖可复现时，保存源码版本、校验和、构建参数、许可证；若要直接提交预编译 deps，需要明确调整忽略策略。sysroot 是设备基础环境，不建议整体提交。

编译时指定 SDK 不会自动完成运行库打包。未来动态库部署建议采用 `bin/` + `lib/`，可执行程序 INSTALL_RPATH 设置为 `$ORIGIN/../lib`，随包动态库设置 `$ORIGIN`，并在设备用 `ldd` 确认实际加载路径；间接动态依赖也要一起核对。当前工程尚未实现完整安装打包流程。glibc 和动态加载器继续使用设备原版本；MPP 仍须兼容设备驱动。

## 7. Process 后端与源码兼容性

```cpp
common::process::Process child; // RK 不传 Asio executor
common::process::ProcessOptions options;
options.executable = "/bin/echo";
options.args = {"hello"};
std::error_code ec;
if (child.Start(options, ec)) child.Wait(ec);
```

RK AsyncWait 回调在子进程回收线程执行；注册时已结束则在调用线程执行。回调不允许抛出异常，不应阻塞等待其他回调。Reset/析构会终止并回收仍运行的直接子进程，不管理整个进程树。GUI/新终端选项返回 operation_not_supported。生命周期操作需调用方串行化，查询、信号与等待可并发。

MediaStreamSession 使用无参构造，内部仍是原有 std::thread 模型，已移除从未使用的 Boost io_context 参数。generic Process 保留原 Boost API，RK Process 使用 std::error_code 与无参构造。
