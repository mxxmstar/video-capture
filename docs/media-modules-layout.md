# 媒体模块目录重构

本轮将媒体代码从顶层 `include/media`、`src/media` 迁移到 `modules/media`。只调整源码组织、包含目录和构建入口，不实现或验证 SDK 导出。

## 目录结构

```text
modules/media/
  CMakeLists.txt
  core/
    include/media/                 # Frame、Packet、Buffer、时间戳等
    src/
    CMakeLists.txt
  encoder/                        # converter、decoder、puller、pusher 同类结构
    include/media/encoder/        # 公共接口、配置
    backend/include/media/encoder/generic/ # 可选后端公开头
    src/generic/                  # FFmpeg 实现
    src/encoder_config.cpp
    CMakeLists.txt
  capture/
  converter/
  decoder/
  puller/
  pusher/
  publisher/
  stream/
  ffmpeg/backend/include/media/generic/ # 后端共享帧/包适配辅助头
  diagnostics/src/                # 日志诊断实现
```

为减少调用方变更，沿用 puller、pusher、stream 命名，分别对应 Input、Output、Pipeline 构建目标。测试迁到各模块自己的 `test/`，顶层 `test/` 已删除；平台依赖查找、工具链和 SDK 文件继续位于 `platform/`。公共进程与渲染代码本轮仍保留原位置。

## 构建边界

- 顶层 CMake 改为 `add_subdirectory(modules/media)`。
- 每个实现模块的 CMake 编译本模块 `src/`；公共头文件随模块放置，调用方继续使用 `#include <media/...>`。
- `Media::Contracts` 是头文件接口集合，汇总各模块公共 `include/`。它不引入 FFmpeg、spdlog、Boost。现有 `common/queue` 头仍从顶层 `include/` 引用，因此源码级媒体模块尚不完全独立于仓库公共头。
- `Media::Core` 保留原有公共配置校验职责，因此 Core CMake 仍编译 encoder/pusher/publisher 的配置实现。此轮未扩大为配置库拆分。
- `backend/include/` 中的头可能使用 AVFrame、AVPacket 等原生类型，只由对应后端目标传播，不加入 Contracts。未来如需发布专用后端开发接口，可以单独选择这些头。
- `Media::Core`、`Media::Encoder`、`Media::Decoder`、`Media::Input`、`Media::Output`、`Media::Publisher`、`Media::Pipeline`、`Media::All` 等目标名保持不变。
- 无 FFmpeg 时仍只构建公共 Core，与上一阶段行为一致。
- 旧的 `include/media`、`src/media` 已移除，未保留双份代码或目录转发。

## 验证方式

完整 x86 构建沿用前一份进展文档中的依赖参数，使用新目录 `build-modules-layout`。保留 AVFrame owner 的配置使用 `build-modules-layout-raw`，添加 `-DMEDIA_RAW_FRAME_BUFFER=ON`。

独立 Core 验证：

```bash
cmake -S modules/media/core/test -B build-modules-core
cmake --build build-modules-core -j4
ctest --test-dir build-modules-core --output-on-failure
```

该验证只检查仓库内公共契约和现有功能，不是 SDK 消费者或安装包验证。SDK 的 install、export、包配置和工程外 find_package 验证均留待后续。

## 模块内测试

每个测试目录有独立 `CMakeLists.txt`，由媒体父 CMake 按 `BUILD_TESTS`、`BUILD_MEDIA_TESTS` 和后端开关注册。

| 测试目录 | 内容 |
| --- | --- |
| `modules/media/core/test` | 公共契约、packet buffer；契约测试可独立配置构建 |
| `modules/media/converter/test` | frame view、转换 |
| `modules/media/ffmpeg/test` | AVFrame owner 契约 |
| `modules/media/pusher/test` | 输出后端、同步/异步发布会话 |
| `modules/media/publisher/test` | 多目标发布 |
| `modules/media/stream/test` | 脚本输入、重连及时间戳策略 |
| `modules/media/pipeline/test` | 跨输入、编解码、转换及发布的完整链路集成测试 |
| `modules/media/capture/test` | V4L2 契约、设备测试及 smoke 工具 |
| `src/common/process/test` | RK POSIX 进程测试 |
| `src/render/test` | OpenGL 渲染测试 |

encoder、decoder、puller 的完整链路目前由 pipeline/test 联合覆盖，未添加空测试占位目录。现有核心功能仍可一次运行 `ctest --test-dir build-modules-layout --output-on-failure`。模块测试可通过 `ctest -R` 按名称筛选；只有 Core 契约入口支持独立顶层配置，其他测试按依赖图随完整工程配置。

V4L2 demo 位于 capture/test/v4l2-demo，通过渲染测试入口按开关构建；真实设备测试保持显式 opt-in。`BUILD_TESTS=OFF` 时不注册测试；`BUILD_MEDIA_TESTS=OFF` 只关闭媒体测试，不影响启用渲染时的渲染测试。

## 本轮验证结果

- x86 默认帧存储：新构建目录 `build-modules-layout`，12/12 通过。
- x86 AVFrame owner 模式：新构建目录 `build-modules-layout-raw`，12/12 通过。
- 模块内独立 Core：`build-modules-core-local`，1/1 通过，无第三方媒体 SDK 依赖。
- `BUILD_TESTS=OFF`：配置成功，CTest 注册 0 项。
- RK 设备、OpenGL 运行及 Windows 构建未在本轮验证；SDK 安装、导出、消费验证按要求暂不进行。
