# NanoPC-T4 ARM64 C++20 交叉编译环境

配置日期：2026-10-28。编译在 Ubuntu 虚拟机中进行，程序在 NanoPC-T4 上运行，不修改设备的系统编译器、glibc 或 MPP 库。

## 1. 环境与目录

| 项目 | 配置 |
| --- | --- |
| 编译主机 | `ub22@192.168.117.131`，Ubuntu 22.04 x86_64 |
| 目标设备 | `pi@192.168.66.6`，RK3399 / NanoPC-T4，Ubuntu 20.04 AArch64 |
| 目标 glibc | 2.31 |
| 目标 libstdc++ | GCC 10.5 系列 |
| 交叉编译器 | 默认 `aarch64-linux-gnu-gcc-12`、`aarch64-linux-gnu-g++-12`，12.3.0；另安装 GCC 10.5 兼容配置 |
| 工具链工作目录 | `/home/ub22/arm64-toolchain` |
| 目标 sysroot | `/home/ub22/arm64-toolchain/sysroots/nanopc-t4` |
| CMake toolchain 文件 | `/home/ub22/arm64-toolchain/cmake/nanopc-t4-gcc12.cmake` |
| 测试工程 | `/home/ub22/arm64-toolchain/examples/hello-cxx20` |

默认使用 GCC 12.3 编译器启用 C++20 语言模式，配合设备的 GCC 10 标准库头文件与运行库，避免升级设备运行环境。本环境实测 `std::span`、concepts、ranges 等特性。标准库能力仍受 GCC 10 版本限制，例如不能假定 `std::format` 可用；并不代表完整实现所有 C++20 库特性。GCC 10.5 兼容配置也保留，但该编译器将 C++20 标记为草案模式（`__cplusplus=201709L`），本例默认使用 GCC 12，要求 `__cplusplus >= 202002L`。

## 2. 安装主机工具与配套文件

登录虚拟机：

```bash
ssh ub22@192.168.117.131
sudo apt-get update
sudo apt-get install -y g++-12-aarch64-linux-gnu g++-10-aarch64-linux-gnu cmake ninja-build rsync file
aarch64-linux-gnu-g++-12 --version
aarch64-linux-gnu-g++-12 -dumpmachine
```

工程中已保存配套文件 `docs/arm64-toolchain/`（toolchain、sysroot 导出脚本、Hello World 示例）。新环境安装后先复制到工作目录：

```bash
TASK_ROOT=/home/ub22/arm64-toolchain
mkdir -p "$TASK_ROOT"
cp -a /home/ub22/mxxmstar/video-capture/docs/arm64-toolchain/. "$TASK_ROOT/"
```

预期目标三元组为 `aarch64-linux-gnu`。APT 安装的 ARM64 库默认面向 Ubuntu 22.04，不能直接视为 Ubuntu 20.04 设备的运行环境。因此还需使用下述目标 sysroot。

## 3. 从目标设备导出 sysroot

已提供 `/home/ub22/arm64-toolchain/export-sysroot.py`。它需要运行在目标设备上，导出设备的头文件、GCC 10 开发库、glibc 启动文件和核心运行库，并包含现有 MPP/RGA 开发文件。

在虚拟机执行，SSH/SCP 按提示输入设备密码，不在脚本或文档中保存密码：

```bash
TASK_ROOT=/home/ub22/arm64-toolchain
mkdir -p "$TASK_ROOT"/{archives,sysroots/nanopc-t4,cmake,examples/hello-cxx20}
scp "$TASK_ROOT/export-sysroot.py" pi@192.168.66.6:/tmp/export-sysroot.py
ssh pi@192.168.66.6 \
  'python3 /tmp/export-sysroot.py /tmp/nanopc-t4-sysroot.tar.gz'
scp pi@192.168.66.6:/tmp/nanopc-t4-sysroot.tar.gz "$TASK_ROOT/archives/"
tar xzf "$TASK_ROOT/archives/nanopc-t4-sysroot.tar.gz" \
  -C "$TASK_ROOT/sysroots/nanopc-t4"
ln -sfn usr/lib "$TASK_ROOT/sysroots/nanopc-t4/lib"
```

本次导出归档保存在 `archives/nanopc-t4-sysroot-20261028.tar.gz`。`lib -> usr/lib` 对应设备的 usrmerge 布局。请保留归档中的目录结构和符号链接，不要用普通文件覆盖链接。

重新生成 sysroot 时，建议提取到一个新目录，验证通过后再切换，避免混入旧文件。更换目标系统、更新 MPP 或运行库后，应重新导出。

这是用于当前目标的精简 sysroot，并非完整根文件系统。Boost、FFmpeg、spdlog 等完整工程依赖仍需单独准备 ARM64 版本；不能链接虚拟机的 x86_64 库。导出脚本要求设备已有 `/usr/lib/gcc/aarch64-linux-gnu/10` 和开发头文件，本设备已具备。

## 4. CMake toolchain 的关键设置

默认文件 `cmake/nanopc-t4-gcc12.cmake` 复用 `cmake/nanopc-t4-gcc10.cmake` 的目标路径设置，并选择 GCC 12 编译器；这两个文件需放在同一目录。配置了：

- 目标系统 Linux、架构 AArch64，以及 GCC 12 交叉编译器。
- `CMAKE_SYSROOT` 指向设备导出的根目录。
- 用 `-nostdinc++` 和 `-isystem` 明确选择设备的 GCC 10 C++ 标准库头文件。
- 用 `-B` 选择设备的 glibc 启动文件；仅设置 `--sysroot` 不能保证覆盖交叉编译器的所有默认搜索路径。
- 用 `-L` 和 `-rpath-link` 指向 sysroot 的目标库。`rpath-link` 仅供链接阶段使用，不会把虚拟机路径作为运行时依赖写入设备程序。
- CMake 查找头文件、库、包时限制在目标根目录；构建工具仍在主机查找。

不要全局覆盖系统 `gcc`/`g++`。每个 CMake 构建目录首次配置时显式传入 toolchain 文件即可；更换工具链时使用新的构建目录。

## 5. 编译 Hello World / C++20 测试

测试源代码为 `examples/hello-cxx20/main.cpp`，验证 ARM64 宏、C++20 模式、`std::span`、concepts、constexpr 和 ranges。

```bash
TASK_ROOT=/home/ub22/arm64-toolchain
cmake -S "$TASK_ROOT/examples/hello-cxx20" \
  -B "$TASK_ROOT/build/hello-gcc12" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TASK_ROOT/cmake/nanopc-t4-gcc12.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$TASK_ROOT/build/hello-gcc12" --verbose
file "$TASK_ROOT/build/hello-gcc12/hello_arm64"
aarch64-linux-gnu-readelf -l "$TASK_ROOT/build/hello-gcc12/hello_arm64" \
  | grep interpreter
aarch64-linux-gnu-readelf --version-info \
  "$TASK_ROOT/build/hello-gcc12/hello_arm64"
```

`file` 应显示 AArch64 ELF，动态加载器应为 `/lib/ld-linux-aarch64.so.1`。检查 `readelf` 的版本需求，确保不要求设备没有的 GLIBC / GLIBCXX 版本。不要在 x86_64 虚拟机上用普通 `ldd` 或直接执行 ARM64 程序。

## 6. 部署到设备并运行

在虚拟机执行：

```bash
TASK_ROOT=/home/ub22/arm64-toolchain
ssh pi@192.168.66.6 'mkdir -p /home/pi/arm64-toolchain-test'
scp "$TASK_ROOT/build/hello-gcc12/hello_arm64" \
  pi@192.168.66.6:/home/pi/arm64-toolchain-test/hello_arm64
ssh pi@192.168.66.6 \
  'chmod +x /home/pi/arm64-toolchain-test/hello_arm64; \
   ldd /home/pi/arm64-toolchain-test/hello_arm64; \
   /home/pi/arm64-toolchain-test/hello_arm64'
```

成功时应输出 Hello World、编译器版本、span/concepts 求和 `10`、ranges 偶数求和 `6`，进程退出码为 `0`。无需替换设备系统库，也无需设置 `LD_LIBRARY_PATH`。

## 7. 工程接入范围

本次只安装编译环境、提供 toolchain、示例及文档，没有改动 `video-capture` 的业务源码或部署该工程主程序。当前工程默认的 vcpkg triplet 为 x64，FFmpeg 路径也需要匹配 ARM64，因此还不能只传入本 toolchain 就认定整个工程已经可以交叉编译。

后续接入时需要准备 ARM64 Boost、spdlog、FFmpeg 等依赖，并按实际构建方式处理 vcpkg chainload / triplet。MPP 后端可链接本设备匹配的 `librockchip_mpp`。这些属于后续工程适配工作。

## 8. 常见问题

| 报错 | 检查方向 |
| --- | --- |
| `Exec format error` | 是否把 x86_64 程序部署到设备，或在虚拟机直接执行 ARM64 程序 |
| `GLIBC_2.34 not found` | 是否错误选择了 Ubuntu 22 的启动文件或库；检查 `-B`、`--sysroot` 和链接日志 |
| `GLIBCXX_x.x.xx not found` | 编译时 C++ 头文件、链接时 libstdc++、设备运行库是否一致 |
| `cannot find crt*.o` | sysroot 是否包含目标 libc 开发文件，`-B` 是否指向正确目录 |
| 缺少 `<span>` | 是否使用 sysroot 中的 GCC 10 C++ 头文件并启用 C++20，而非设备默认 GCC 9 头文件 |
| MPP 编译成功但初始化失败 | 需要进一步检查设备 MPP 驱动、权限及芯片支持；与 Hello World 工具链验证不同 |

## 9. 本次实测记录

已在虚拟机使用 GCC 12.3 编译，生成 AArch64 ELF，并在 `192.168.66.6` 上执行成功，退出码为 `0`：

```text
Hello World from NanoPC-T4 ARM64!
C++ standard: 202002
Compiler: 12.3.0
std::span + concepts sum: 10
std::ranges even sum: 6
```

本例可执行文件要求的版本为 `GLIBC_2.17`、`GLIBCXX_3.4` 和 `GLIBCXX_3.4.9`，均在设备现有库支持范围内。此结论只针对本例；后续新增依赖后需要重新检查二进制和实际部署结果。

`ldd` 确认加载设备原有的 `/lib/aarch64-linux-gnu/libstdc++.so.6`、`libc.so.6`、`libm.so.6`、`libgcc_s.so.1`，未部署额外系统运行库。

二进制位置：

- 虚拟机：`/home/ub22/arm64-toolchain/build/hello-gcc12/hello_arm64`
- 设备：`/home/pi/arm64-toolchain-test/hello_arm64`
- SHA-256：`4ef8e1c5cdecec0d965f4690a41cbdbd1429c4bb69accb30fa68ed81789238c8`

原始设备验证输出另存于工具链工作目录下的 `device-test.txt`。
