# Rockchip Pusher：工程内 MP4 → RTSP

本轮只推进 Pusher，`src/main.cpp` 不变。FFmpeg 的封装/推流实现可以在 generic 与 Rockchip 间共享，不属于某一芯片；只有 Pusher 的实现目录从 generic 改为 ffmpeg，其它模块不做目录迁移。

## 实际调用链

```text
test_mp4_rtsp_pusher（libavformat 读取 MP4、按 DTS 实时投递）
  -> CreateMediaPusher() / IPusher
  -> FFmpegPusher::Open / Push / Close
  -> FFmpegMuxer
  -> RTSP/TCP -> Windows ZLMediaKit
```

测试不自行创建输出 AVFormatContext、不直接调用输出 write_header/write_frame。它将输入包转换为普通字节型 MediaPacket，经工程 Pusher 转成输出包；这也覆盖未来 MPP 编码器提供字节包的接入路径。测试本身不是 Puller 模块实现。

目前遵循既有 IPusher 能力：单路 H.264 视频；MP4 的音频和其它轨道不发送。复制编码数据，不解码/转码，不调用 MPP。保持输入 SPS/PPS extradata、PTS/DTS 差值、关键帧标记和时间基；按 DTS 实时限速，EOF 结束。可用最后一个参数限制发送秒数。没有新增断线重连或循环播放功能。

## 构建开关与目录

- `MEDIA_FFMPEG_FULL=OFF`：不构建完整 converter/encoder/decoder/puller 等媒体后端。
- `MEDIA_FFMPEG_ROCKCHIP=ON`：单独启用共享 Pusher 和 Session。
- `BUILD_MEDIA=ON`、`BUILD_TESTS=ON`、`BUILD_MEDIA_TESTS=ON`：构建本测试和已有 Pusher 测试。
- `FFMPEG_ROOT`：目标 FFmpeg SDK 前缀，统一链接设备端 avformat/avcodec/avutil/avdevice/avfilter/swscale/swresample 七个库；功能范围由代码模块开关决定。
- `ROCKCHIP_DEPS_ROOT`：继续提供已交叉编译的 spdlog。

```text
modules/media/pusher/src/ffmpeg/
modules/media/pusher/backend/include/media/pusher/ffmpeg/
modules/media/pusher/test/test_mp4_rtsp_pusher.cpp
cmake/FFmpeg.cmake
```

共享的 packet 适配头以及 Pusher 测试改用 avcodec.h，以兼容板端 FFmpeg4.2。HTTP429 错误映射只在对应 FFmpeg 提供该常量时启用。generic 新版 FFmpeg 继续使用同一 Pusher 实现。

## 当前 RK3399 SDK

使用设备已有 FFmpeg4.2 的开发头文件、动态库以及链接所需的间接依赖，无需升级设备系统库。当前主机位置：

`platform/rockchip/deps/aarch64/ffmpeg-board/`

其中 `include/` 与 `lib/` 是七个直接依赖；`link-deps/` 保存从同一设备导出的间接 DSO，仅供交叉链接查找，不部署，也不作为应用 RPATH。目录由现有 deps 忽略规则排除，不提交二进制库。

可在设备执行 `platform/rockchip/export-ffmpeg-sdk.py OUTPUT.tar.gz` 导出 SDK，再复制至编译主机、解压到新的 FFMPEG_ROOT。脚本针对当前 NanoPC-T4 ARM64 布局，使用设备 ldd，不能在 x86 主机运行，也不适用于 RV1126；会记录库来源、SHA256，拒绝覆盖旧归档。不要运行来源不可信二进制的 ldd。

## Ubuntu 交叉编译

当前 `config/build.local.sh` 的 Rockchip 参数已添加 OUTPUT 开关和上述 SDK 路径。

```bash
cd /home/ub22/mxxmstar/embedded-device
bash scripts/build-menu.sh --platform rockchip --type Debug --action configure
cmake --build build/menu/rockchip/Debug --target test_mp4_rtsp_pusher -j2
```

产物：`build/menu/rockchip/Debug/bin/test_mp4_rtsp_pusher`。只构建该目标，不需要修改 main 或启动 execution。

## 设备运行与播放

本轮部署位置是独立测试目录，没有覆盖设备原应用和系统库：

```bash
cd /home/pi/embedded-device-tests/project-pusher
./test_mp4_rtsp_pusher /home/pi/Videos/demo.mp4 \
  rtsp://192.168.66.123:554/live/rk3399_pusher
# 或末尾加 45，仅发送 45 秒
```

推流时可播放 `rtsp://192.168.66.123:554/live/rk3399_pusher`。测试结束后流注销，再次播放前需要重新运行。ZLMediaKit 使用用户已启动的实例，不由测试启动。

## 验证（2026-10-30）

- 工程内 Pusher、PusherSession、AsyncPusherSession 及测试使用 GCC12 + NanoPC-T4 sysroot 交叉编译。
- 三个既有 Pusher 测试在 RK3399 上全部通过。
- 工程 IPusher 推流 45 秒，正常关闭；重新从 ZLMediaKit 拉流识别为 H.264 1920×800，连续解码 5 秒成功。
- generic 平台同一输出模块使用 FFmpeg8.1 编译，三个 Pusher CTest 全部通过。
- 旧 `test/mp4_rtsp/` 是此前单独 libavformat 的连通性验证，不作为本轮模块验收；应运行本页的 test_mp4_rtsp_pusher。

尚未验证 RV1126/RK3588，尚未接音频、多轨、MPP 编码、ISP 或采集。这些不由本轮推流成功推断。

FFmpeg 依赖统一由 cmake/FFmpeg.cmake 管理，链接设备端七个标准组件。MEDIA_FFMPEG_ROCKCHIP 仅控制当前代码模块构建范围，不裁剪 SDK 链接组件。完整 SDK 导出脚本为 platform/rockchip/export-ffmpeg-sdk.py。
