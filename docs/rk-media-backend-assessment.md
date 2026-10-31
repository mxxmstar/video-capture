# RK3399 / RV1126 无 FFmpeg 媒体后端评估

检查日期：2026-10-30。目标：维护同一套工程源码，分别构建 RK3399 和 RV1126 程序，应用媒体链路不依赖 FFmpeg。

## RV1126 实机确认

通过 root Telnet 登录 192.168.66.82，执行只读环境检查；未停止 ipsys/alg，未安装、替换库或改动驱动。

| 项目 | 实测 |
| --- | --- |
| 架构 | ARMv7 / armv7l，32 位，Cortex-A7 |
| 系统 | Buildroot 2018.02-rc3-02947-gb62761c4e8 |
| 内核 | 4.19.111，2025-05-23 构建 |
| libc / 加载器 | glibc 2.28；/lib/ld-linux-armhf.so.3 |
| C++ 运行库 | libstdc++.so.6.0.25，GCC8 代运行库；未确认目标工具链版本 |
| MPP | /usr/lib/librockchip_mpp.so.1；内嵌版本提交 7ae6041，2021-02-21，rv1126_release 分支标识 |
| RGA | /usr/lib/librga.so.2.0.0 |
| 媒体封装 | /usr/lib/libeasymedia.so.1.0.1 |
| ISP | /usr/lib/librkaiq.so |
| 音频 | /usr/lib/libasound.so.2.0.0 |
| 硬件节点 | /dev/mpp_service、/dev/rga、/dev/dri/card0、renderD128 |
| 摄像头节点 | rkisp_mainpath/video5、rkisp_selfpath/video6、rkispp_m_bypass/video18、rkispp_scale0/video19 等；不能假定 video0 是可直接用的 YUV 输出 |

/proc/ipsys 和 alg 对应进程 maps 确认这些进程正在加载 MPP/RGA，ipsys 还加载 EasyMedia/RKAIQ/ALSA。这不是新工程的硬件功能实测。

## EasyMedia 的隐含 FFmpeg 依赖

用设备动态加载器 --list 检查 libeasymedia.so.1.0.1，依赖解析包含 libavformat.so.58、libavcodec.so.58、libswresample.so.3、libavutil.so.56，以及 Live555、MPP、RGA、RKAIQ、ALSA。

若要求应用进程完全不加载 FFmpeg，则不能直接链接该现成 EasyMedia。即使业务仅调用 RK_MPI_VENC，现有共享库依赖仍然存在。可选择：

- 优先评估直接使用设备 MPP、RGA、V4L2、RKAIQ、ALSA，保持应用不链接 EasyMedia。
- 若 RKMedia 的采集绑定等功能确有必要，取得与固件匹配的 SDK 源码，检查能否关闭 FFmpeg 模块并重建为工程私有 EasyMedia；必须验证重建产物的完整动态依赖。当前尚未验证可裁剪性。

板上存在 FFmpeg 库本身不影响新应用完全不链接它；不需要删除系统库。

## 推荐共享边界

业务层共享 MediaFrame/MediaPacket、配置、回调、队列、时间戳、重连和链路调度；不得引入 AVFrame/AVPacket。编码/解码优先抽象为共有的 Rockchip MPP 后端，使用两套配套头文件与库共有的 API。能力差异（格式、分辨率、码控、解码能力等）通过能力查询和板级配置表达。

采集/ISP、缓冲区分配与同步、显示和 RGA 扩展 API 保留板级适配。RV1126 的 ISP/RKISPP/RKAIQ 流程与 RK3399 的输入方案不能仅靠改路径统一；RK3399 具体输入仍需按选定设备验证。

建议组织：

```text
modules/media/*/src/rockchip/       # MPP 编解码、RGA 适配，尽可能共享
modules/media/capture/src/rockchip/ # 采集的公共逻辑和板级实现
platform/rockchip/rk3399/           # ARM64 工具链、sysroot、能力/依赖配置
platform/rockchip/rv1126/           # ARMv7 hard-float 工具链、sysroot、能力/依赖配置
```

这是建议布局，本次没有调整源码目录。

RK3399 沿用现有 NanoPC-T4 Ubuntu20.04 ARM64 / glibc2.31 配置（用户确认环境未变）；本次未重新登录该板。RV1126 使用独立 ARM32 配置，不能复用 ARM64 sysroot、库或程序。当前 cmake/Platform.cmake 的 rockchip 校验仅允许 ARM64，需要扩展为按板型校验架构。

现工程要求 C++20，并使用 std::span；RV1126 的 GCC8 代标准库不能直接满足该需求。必须单独验证可用于 glibc2.28 的较新 ARM32 工具链和私有 C++ 运行库方案，或限定兼容层的 C++ 标准/替代类型；仅改 -std=c++20 不足以解决标准库问题。不要全局替换设备运行库。

现 modules/media/CMakeLists.txt 在 MEDIA_FFMPEG_FULL=OFF 时提前 return，capture、encoder、decoder、publisher、stream 等不构建；需要按实际后端拆分构建条件，保留上层调度。BackendHandle 当前只有 NONE/FFMPEG，硬件帧需补充 RK 后端句柄或不透明所有者、DMA-BUF 描述、stride/offset 和资源释放规则。不能把硬件 fd 当作 CPU plane 指针。

## 库来源与实施顺序

优先使用各设备 SDK 配套的 MPP/RGA/RKAIQ，而不是拉 GitHub 最新版统一替换两台设备。相同源码可链接不同 ABI/版本库；对新版专属 API 做能力判断或适配。确有缺陷或缺少必需 API 时，才固定上游提交编译为工程私有库并上板验证。

1. 准备 RV1126 配套 SDK/sysroot，先验证 C++20 基础程序和运行库 ABI；RK3399 保持现配置。
2. 做不依赖 FFmpeg/EasyMedia 的 MPP 最小编码测试：NV12 文件输入 -> H.264 裸码流输出，分别编译、部署到两台设备。用离线码流避免占用已有摄像头服务。处理 SPS/PPS、IDR、码控、时间戳、drain 和资源回收。
3. 验证 MPP 解码和异常恢复，再接入实际采集、ISP 和 RGA；获取设备资源前确认与现有服务的使用关系。
4. 文件裸流通路完成后，再明确网络协议或容器格式，选用独立的拉推流/封装库。MPP 不提供 RTSP/RTMP、MP4 封装或通用音频编解码，原有 FFmpeg 对应功能需要替代实现。
5. 先保证有限复制的功能正确，再推进 DMA-BUF 零拷贝与同步。验收时除链路功能外，核对应用的完整动态依赖中没有 libav* / libsw*。

本次仅评估与记录环境，没有改动构建或媒体业务代码。
