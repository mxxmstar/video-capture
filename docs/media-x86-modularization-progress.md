# x86 媒体模块拆分进展

更新时间：2026-10-29。

目录更新：媒体源码已迁到 `modules/media/<模块>/{include,backend/include,src}`，测试已迁到各模块 `test/`，顶层 `test/` 已删除。当前目录与测试命令见 [媒体模块目录重构](media-modules-layout.md)。下文目录示例记录第一阶段拆分时的结构。
工程：`/home/ub22/mxxmstar/embedded-device`。

## 本轮结果

已把媒体实现从 `common_lib` 中移出，按输入、转换、编码、解码、输出、发布策略、输入会话分别建立静态库及独立 CMakeLists。x86 Linux 使用现有 FFmpeg 8.1 SDK 完成构建和自动回归。`platform/` 继续只管理构建、工具链、SDK 与依赖，不放 C++ 实现。

本轮是可编译、可验证的模块化第一阶段。它没有把 `execution` 改成完整的媒体业务程序：主程序仍保留现有入口，端到端能力由自动测试验证。没有部署至 RK 设备，也没有实现 MPP/RGA 后端。

## 模块及依赖

| 目标 / 别名 | 职责 | 主要依赖 |
| --- | --- | --- |
| `media_core` / `Media::Core` | Frame、Packet、Buffer、时间戳换算、公共配置校验及接口 | C++ 标准库 |
| `media_diagnostics` | 原有 Dump 诊断实现 | Core、spdlog |
| `media_converter` / `Media::Converter` | x86 音视频转换及 FFmpeg 帧适配 | FFmpeg、Diagnostics |
| `media_encoder` / `Media::Encoder` | x86 软件编码 | Converter |
| `media_decoder` / `Media::Decoder` | x86 软件解码 | Converter |
| `media_input` / `Media::Input` | FFmpeg 文件/网络输入 | FFmpeg、Diagnostics |
| `media_output` / `Media::Output` | FFmpeg 封装输出及默认 Pusher 工厂 | FFmpeg、Diagnostics |
| `media_pusher_session` | 同步时间轴策略、异步队列与停止策略 | Output、Threads |
| `media_publisher` / `Media::Publisher` | 多输出目标聚合 | PusherSession |
| `media_pipeline` / `Media::Pipeline` | 输入会话、重连及时间戳策略 | Diagnostics、Threads |
| `media_capture` / `Media::Capture` | Linux V4L2 采集与帧租约 | Core、spdlog、Threads |
| `media_modules` / `Media::All` | 上述模块的便捷聚合入口 | 由所启用模块决定 |

`common_lib` 只保留公共进程等原有功能，不再隐式提供媒体实现。需要媒体的消费者必须明确链接相应目标。测试也按具体模块链接，而不统一依赖 `common_lib`。x86 的进程实现仍使用 Boost，因此构建整个工程时平台配置仍会查找 Boost；这不代表 `media_core` 或各媒体目标依赖 Boost。独立 Core 验证工程完全不查找这些包。

当前 `MEDIA_FFMPEG_BACKEND=OFF` 只启用 Core；Capture、Pipeline 等非 Core 模块尚未单独支持无 FFmpeg 的配置组合。RK 默认关闭该开关，避免误用设备 FFmpeg 4.2 软件链路。显式要求 RK 启用 FFmpeg 后端会报清晰错误。

## 目录与接口迁移

公共接口保留在模块目录中，例如：

```text
include/media/media_frame.h
include/media/media_packet.h
include/media/media_timestamp.h
include/media/converter/i_frame_converter.h
include/media/encoder/i_encoder.h
include/media/decoder/i_decoder.h
include/media/puller/i_puller.h
include/media/pusher/i_pusher.h
```

FFmpeg 具体头文件与实现下沉到 `generic/`：

```text
include/media/encoder/generic/ffmpeg_encoder.h
src/media/encoder/generic/ffmpeg_encoder.cpp
include/media/converter/generic/media_frame_converter.h
src/media/converter/generic/media_frame_converter.cpp
include/media/generic/ffmpeg_packet_utils.h
```

其他 FFmpeg decoder、puller、pusher、muxer、音视频 converter 同样迁移。共享 FFmpeg 帧/包适配工具统一位于 `include/media/generic/`。旧 include 路径不再保留，工程内调用已更新，工程外消费者需要同步调整。

新增 `IFrameConverter`，只接受公共转换配置及 `MediaFrame`；原 `MediaFrameConverter` 是 generic 下的 FFmpeg 实现，并实现该接口。AVFrame 静态适配辅助函数只在 generic 头中可见。原来未实现的 OpenCV/SIMD 后端选择被移除，调用方通过创建相应实现来选择后端，不再设置 `config.backend`。

`MediaPacket`、`MediaStreamInfo` 头不再包含日志库；`Dump()` 实现位于可选的 `media_diagnostics`。只链接 Core 的程序可以使用数据和配置接口；如果主动调用 `Dump()`，需要额外链接 Diagnostics。

## 时间戳和所有权约定

- `MediaPacket.pts/dts/duration` 使用 `time_base` 的 tick；帧时间仍使用微秒。`kNoTimestamp` 表示缺失，0 是有效时间点。
- 公共字段是跨模块时间戳的依据。输入会话可以归一化公共字段，但不再直接改写共享 AVPacket。
- FFmpegDecoder 在适配时创建 AVPacket 引用副本，用公共字段覆盖时间戳再换算时间基。即使源时间基与解码器一致，也不会误用原生对象中的旧时间戳。
- FFmpegPusher 同时接受有效的 FFmpeg 原生包与普通字节包。普通字节包在适配层复制成有 FFmpeg 所需 padding 的 AVPacket，再进入 muxer。原生包使用引用副本，保留 side data。
- 异步队列复制公共元数据并共享 owner-backed buffer，保留数据与原生 side data 的生命周期，不再强制要求 AVPacket。发布后，生产者不得修改共享 payload 或原生对象；该接口不承诺对外部写操作建立隔离快照。
- 公共时间基换算由 `RescaleMediaTimestamp` 完成，整数最近舍入、中点远离零；非法时间基或超出 int64 可表示范围返回 `kNoTimestamp`。实现不依赖 FFmpeg 和编译器专用 128 位整数。

Pusher 默认实现通过 `CreateMediaPusher()` 工厂取得。工厂当前位于 `pusher/generic/`，返回 FFmpegPusher；会话策略源文件不再直接包含 FFmpegPusher。后续可以替换工厂实现。当前 Publisher 的默认会话仍通过链接目标选择后端，尚未提供运行时插件注册。

## 构建与复现

以下是虚拟机上本轮使用的 x86 配置。依赖路径可以按本机环境调整，FFmpeg SDK 需要包含软件 H.264 编码器 libx264。

```bash
cd /home/ub22/mxxmstar/embedded-device
cmake -S . -B build-x86-modules \
  -DEMBEDDED_PLATFORM=generic \
  -DEMBEDDED_USE_VCPKG=OFF \
  -DCMAKE_PREFIX_PATH=/home/ub22/vcpkg_env/vcpkg_installed/x64-linux \
  -DFFMPEG_ROOT=/home/ub22/vcpkg_env/ffmpeg-n8.1-latest-linux64-gpl-shared-8.1 \
  -DBUILD_RENDER=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-x86-modules -j4
ctest --test-dir build-x86-modules --output-on-failure --timeout 30
```

只编译某个模块，例如：

```bash
cmake --build build-x86-modules --target media_encoder -j4
```

验证 Core 不需要任何第三方 SDK：

```bash
cmake -S modules/media/core/test -B build-media-core-only
cmake --build build-media-core-only -j4
ctest --test-dir build-media-core-only --output-on-failure
```

验证保留 AVFrame owner 的路径：使用新的构建目录 `build-x86-modules-raw`，在完整配置命令中额外添加 `-DMEDIA_RAW_FRAME_BUFFER=ON`，然后构建并运行 ctest。两个构建目录相互独立。

## 验证情况

环境：虚拟机 Ubuntu x86_64、GCC 12.3、FFmpeg 8.1 SDK。Debug 构建。

| 配置 | 结果 |
| --- | --- |
| 默认帧存储 `MEDIA_RAW_FRAME_BUFFER=OFF` | 12/12 通过 |
| 保留 AVFrame owner `MEDIA_RAW_FRAME_BUFFER=ON` | 12/12 通过 |
| 独立 Core、无 FFmpeg/spdlog/Boost 包查找 | 1/1 通过 |

另外修复了异步发布队列的一处丢失唤醒：通知方与等待方使用同一 wait mutex 同步，等待谓词在 lifecycle mutex 下读取队列计数。测试 Mock 的跨线程可变字段改为 atomic，避免测试自身的数据竞争。修复后，两种帧存储配置各连续重复该测试 50 次均通过，再运行完整回归也均为 12/12。

覆盖内容：

- 公共接口头文件、buffer 所有权、时间戳非法值和溢出边界。
- 10,000 组确定性随机时间基换算与 FFmpeg `av_rescale_q` 对照。
- 10 帧合成 I420 数据 → libx264 编码 → Publisher 双目标 MP4 输出 → FFmpegPuller 读取 → FFmpegDecoder 解码 → Converter 缩放到 8×8 RGB24，验证帧数、尺寸、像素格式与 PTS。
- 上述发布链路混用原生 AVPacket 与普通字节载荷，验证输出时间轴、时长和上游原生包不被发布方改写。
- 解码测试故意使原生包时间戳与公共元数据不一致，检查适配层使用公共元数据。
- 发布会话、多目标发布、异步队列、输入重连及脚本输入状态转换。
- Frame view、raw AVFrame owner 和无设备 V4L2 采集契约。

V4L2 真实摄像头、vivid 故障注入、RTSP 外部服务器、OpenGL 显示、Windows 编译及 RK 运行未在本轮验证。设备测试仍需显式开启对应选项。

## 过期测试和占位代码清理

删除三个固定依赖外部环境的旧测试/示例：

- `test/common/test_process.cpp`：硬编码 Windows MediaServer 路径，不适合自动回归。
- `test/media/test_ffmpeg_puller_decoder_converter_encoder.cpp`：硬编码外部 RTSP 地址的综合演示。
- `test/media/test_media_stream_session.cpp`：硬编码外部 RTSP 地址的旧会话演示。

保留脚本输入、Mock、合成数据等可复现测试，媒体回归改为 CMake 显式注册。RK POSIX 进程测试仍保留。删除未实现且原本就排除编译的 `stream_source.h/.cpp` 占位代码；后续流水线应基于明确契约实现，不能把该占位类视为已有功能。

## 后续推进顺序

1. 继续拆分后端配置：当前公共配置仍保留部分 FFmpeg 选项数据、`sws_flags` 和兼容的后端标记。这些类型不引用 SDK，但配置语义尚未完全中立，后续应迁到 generic 配置或明确的后端扩展参数中。
2. 将 Capture、Pipeline、发布策略的构建开关进一步独立化，使其能够搭配不同后端单独构建；目前已形成独立库，但默认发布会话仍通过 Output 工厂关联具体后端。
3. 定义完整流水线的运行与停止屏障、EOS、背压、错误传播与 Flush 次序，并连接实际应用入口。本轮没有实现任意 Graph 调度器。
4. 在 `include/media/{encoder,decoder,converter}/rockchip/` 和对应 `src/` 中实现 MPP/RGA 后端，复用公共 Frame/Packet/接口。根据设备实际缓冲区能力补充 DMA-BUF、同步及硬件帧所有权契约。
5. 在设备端验证采集 → MPP 编码 → 输出链路，先做单路和稳定关闭，再扩展双摄像头及性能统计。
