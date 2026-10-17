#include "render/video/opengl_video_renderer.h"
#include "media/converter/media_frame_converter.h"
#include "media/simple_buffer.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/dma-heap.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <poll.h>
#include <algorithm>
#include <limits>
#define REQ_BUF_COUNT 4

struct VideoBuffer {
    void*  start   = nullptr;
    size_t length  = 0;

    VideoBuffer() = default;

    ~VideoBuffer() {
        if (start && start != MAP_FAILED) {
            munmap(start, length);
        }
    }

    VideoBuffer(const VideoBuffer&) = delete;
    VideoBuffer& operator=(const VideoBuffer&) = delete;

    VideoBuffer(VideoBuffer&& other) noexcept
        : start(other.start), length(other.length) {
        other.start  = nullptr;
        other.length = 0;
    }

    VideoBuffer& operator=(VideoBuffer&& other) noexcept {
        if (this != &other) {
            if (start && start != MAP_FAILED) {
                munmap(start, length);
            }
            start  = other.start;
            length = other.length;
            other.start  = nullptr;
            other.length = 0;
        }
        return *this;
    }
};

struct MultiPlaneVideoBuffer {
    struct Plane {
        void*  start  = nullptr;
        size_t length = 0;
    };
    std::vector<Plane> planes;

    ~MultiPlaneVideoBuffer() {
        for (auto& p : planes) {
            if (p.start && p.start != MAP_FAILED) {
                munmap(p.start, p.length);
            }
        }
    }

    MultiPlaneVideoBuffer() = default;
    MultiPlaneVideoBuffer(const MultiPlaneVideoBuffer&) = delete;
    MultiPlaneVideoBuffer& operator=(const MultiPlaneVideoBuffer&) = delete;
    MultiPlaneVideoBuffer(MultiPlaneVideoBuffer&&) noexcept = default;
    MultiPlaneVideoBuffer& operator=(MultiPlaneVideoBuffer&&) noexcept = default;
};

struct ExportedBuffer {
    int dma_fds[VIDEO_MAX_PLANES];
    int num_planes = 0;
    ExportedBuffer() { std::fill(std::begin(dma_fds), std::end(dma_fds), -1); }
    ExportedBuffer(const ExportedBuffer&) = delete;
    ExportedBuffer& operator=(const ExportedBuffer&) = delete;
    ~ExportedBuffer() {
        for (int fd : dma_fds) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }
};


namespace {

void xioctl(int fd, unsigned long request, void* arg, const char* name) {
    if (ioctl(fd, request, arg) == -1) {
        throw std::runtime_error(std::string(name) + ": " + strerror(errno));
    }
}

// ---- 共享工具函数 ----

void PrintCapability(const v4l2_capability& cap) {
    std::cout << "driver:  " << cap.driver  << '\n';
    std::cout << "card:    " << cap.card    << '\n';
    std::cout << "bus:     " << cap.bus_info << '\n';
    std::cout << "capture: " << ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? "YES" : "NO") << '\n';
    std::cout << "stream:  " << ((cap.capabilities & V4L2_CAP_STREAMING)     ? "YES" : "NO") << '\n';
}

uint32_t DeviceCaps(const v4l2_capability& cap) {
    return (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
}

// 申请缓冲区（单平面 / 多平面通用）
unsigned RequestBuffers(int fd, v4l2_buf_type type, unsigned count) {
    v4l2_requestbuffers req{};
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = count;
    xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");
    std::cout << "buffers: " << req.count << '\n';
    return req.count;
}

// 多平面 QBUF 入队
void QueueMultiPlaneBuffers(int fd, unsigned count, int num_planes) {
    for (unsigned i = 0; i < count; ++i) {
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = static_cast<__u32>(num_planes);
        buf.m.planes = mplanes;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
    }
}

// STREAMON
void StartStreaming(int fd, v4l2_buf_type type) {
    xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
    std::cout << "stream ON\n";
}

// STREAMOFF + close
void StopStreamingAndClose(int fd, v4l2_buf_type type) {
    xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    close(fd);
}

// 时间戳时钟类型（仅首帧打印）
void PrintTimestampClock(const v4l2_buffer& buf, int frame_count) {
    if (frame_count != 0) return;
    if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
        std::cout << "timestamp clock: CLOCK_MONOTONIC\n";
    else if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_COPY)
        std::cout << "timestamp clock: hardware copy (raw sensor timestamp)\n";
    else
        std::cout << "timestamp clock: CLOCK_REALTIME (may drift with NTP)\n";
}

// V4L2 timeval → 微秒
int64_t TimevalToUs(const timeval& tv) {
    return static_cast<int64_t>(tv.tv_sec) * 1'000'000LL + tv.tv_usec;
}

// 渲染器初始化（失败时自动 STREAMOFF + close）
bool InitRenderer(render::OpenGLVideoRenderer& renderer, int width, int height,
                  const char* title, int fd, v4l2_buf_type type) {
    render::RenderConfig cfg;
    cfg.window_width  = width;
    cfg.window_height = height;
    cfg.title         = title;
    cfg.vsync         = false;
    if (!renderer.Init(cfg)) {
        std::cerr << "renderer init failed: " << renderer.LastError() << '\n';
        StopStreamingAndClose(fd, type);
        return false;
    }
    return true;
}

// ---- 多平面格式设置 ----

struct MultiPlaneFormatInfo {
    int width = 0, height = 0, num_planes = 0, stride = 0;
    v4l2_format fmt = {};
};

MultiPlaneFormatInfo SetupMultiPlaneFormat(int fd, int req_w = 640, int req_h = 480) {
    MultiPlaneFormatInfo info;
    info.fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    info.fmt.fmt.pix_mp.width       = req_w;
    info.fmt.fmt.pix_mp.height      = req_h;
    info.fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    info.fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;

    xioctl(fd, VIDIOC_S_FMT, &info.fmt, "VIDIOC_S_FMT");

    if (info.fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M)
        std::cout << "driver returned different pixfmt: " << (int)info.fmt.fmt.pix_mp.pixelformat << '\n';
    if (info.fmt.fmt.pix_mp.width != req_w || info.fmt.fmt.pix_mp.height != req_h)
        std::cout << "driver adjusted size to " << info.fmt.fmt.pix_mp.width << 'x' << info.fmt.fmt.pix_mp.height << '\n';

    info.width      = info.fmt.fmt.pix_mp.width;
    info.height     = info.fmt.fmt.pix_mp.height;
    info.num_planes = info.fmt.fmt.pix_mp.num_planes;
    info.stride     = info.fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    std::cout << "format:  " << info.width << 'x' << info.height
              << "  planes=" << info.num_planes
              << "  stride=" << info.stride
              << "  size[0]=" << info.fmt.fmt.pix_mp.plane_fmt[0].sizeimage << '\n';

    for (int p = 0; p < info.num_planes; ++p) {
        std::cout << "  plane[" << p << "]"
                  << " bytesperline=" << info.fmt.fmt.pix_mp.plane_fmt[p].bytesperline
                  << " sizeimage="    << info.fmt.fmt.pix_mp.plane_fmt[p].sizeimage << '\n';
    }
    return info;
}

// ---- 轮询 / DQBUF 辅助 ----

enum class PollStatus { Ready, Timeout, Error, DeviceError };

PollStatus PollDevice(int fd, int timeout_ms) {
    pollfd pfd{fd, POLLIN, 0};
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)  return (errno == EINTR) ? PollStatus::Timeout : PollStatus::Error;
    if (ret == 0) return PollStatus::Timeout;
    if (pfd.revents & (POLLERR | POLLHUP)) return PollStatus::DeviceError;
    if (!(pfd.revents & POLLIN))           return PollStatus::Timeout;
    return PollStatus::Ready;
}

// DQBUF（多平面），返回 false 表示应退出循环
bool DequeueMultiPlaneBuffer(int fd, v4l2_buffer& buf, v4l2_plane* planes,
                             int num_planes, render::OpenGLVideoRenderer& renderer,
                             bool& failed) {
    buf = {};
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.length   = static_cast<__u32>(num_planes);
    buf.m.planes = planes;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN || errno == EINTR) { renderer.PollEvents(); return true; }
        failed = true;
        std::cerr << "DQBUF error: " << strerror(errno) << '\n';
        return false;
    }
    if (buf.flags & V4L2_BUF_FLAG_ERROR) {
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
        return true;  // 丢弃错误帧，继续
    }
    return true;
}

} // namespace

// ================================================================
//  Demo 1: 单平面 YUYV 采集 → I420 转换 → OpenGL 渲染
// ================================================================
int v4l2_demo(int argc, char* argv[]) {
    const char* device = (argc > 1) ? argv[1] : "/dev/video0";

    int fd = open(device, O_RDWR);
    if (fd < 0) { std::cerr << "open " << device << " failed: " << strerror(errno) << '\n'; return 1; }

    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");
    PrintCapability(cap);
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << "device does not support capture/streaming\n"; close(fd); return 1;
    }

    v4l2_format fmt{};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = 1920;
    fmt.fmt.pix.height      = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
        std::cout << "driver returned different pixfmt: " << (int)fmt.fmt.pix.pixelformat << '\n';

    int width  = fmt.fmt.pix.width;
    int height = fmt.fmt.pix.height;
    int stride = fmt.fmt.pix.bytesperline;
    std::cout << "format:  " << width << 'x' << height
              << "  stride=" << stride << "  size=" << fmt.fmt.pix.sizeimage << '\n';

    unsigned buf_count = RequestBuffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, REQ_BUF_COUNT);

    std::vector<VideoBuffer> buffers(buf_count);
    for (unsigned i = 0; i < buf_count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");
        buffers[i].length = buf.length;
        buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED)
            throw std::runtime_error(std::string("mmap: ") + strerror(errno));
        std::cout << "buf[" << i << "] addr=" << buffers[i].start << " len=" << buffers[i].length << '\n';
    }

    for (unsigned i = 0; i < buf_count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    StartStreaming(fd, type);

    render::OpenGLVideoRenderer renderer;
    if (!InitRenderer(renderer, width, height, "V4L2 Raw + OpenGL Demo", fd, type)) return 1;

    MediaFrameConverter converter;
    {
        MediaFrameConverterConfig conv_cfg;
        conv_cfg.video = {static_cast<int32_t>(width), static_cast<int32_t>(height), PixelFormat::kI420, 2};
        if (!converter.Open(conv_cfg)) throw std::runtime_error("converter open failed");
    }

    int frame_count = 0;
    std::cout << "capturing... (press ESC to quit)\n";

    while (!renderer.ShouldClose()) {
        PollStatus ps = PollDevice(fd, 100);
        if (ps == PollStatus::Error)         { std::cerr << "poll error\n"; break; }
        if (ps == PollStatus::DeviceError)   { std::cerr << "device error\n"; break; }
        if (ps != PollStatus::Ready)         { renderer.PollEvents(); continue; }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) { renderer.PollEvents(); continue; }
            std::cerr << "DQBUF error: " << strerror(errno) << '\n'; break;
        }

        size_t used = buf.bytesused ? buf.bytesused : fmt.fmt.pix.sizeimage;
        auto yuyv_frame = MediaFrame::CreateCompact(
            static_cast<const unsigned char*>(buffers[buf.index].start), used,
            PixelFormat::kYUYV422, width, height, stride);

        PrintTimestampClock(buf, frame_count);
        yuyv_frame->time.pts_us = TimevalToUs(buf.timestamp);

        std::shared_ptr<MediaFrame> i420_frame;
        if (!converter.Convert(*yuyv_frame, i420_frame)) {
            std::cerr << "conversion failed at frame " << frame_count << '\n'; break;
        }
        if (!renderer.Render(*i420_frame)) {
            std::cerr << "render failed: " << renderer.LastError() << '\n';
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF"); break;
        }
        ++frame_count;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
        renderer.PollEvents();
    }

    std::cout << "stopping... rendered " << frame_count << " frames\n";
    StopStreamingAndClose(fd, type);
    return 0;
}

// ================================================================
//  Demo 2: 多平面 YUV420M (mmap) → I420 → OpenGL 渲染
// ================================================================
int v4l2_multiplane_demo(int argc, char* argv[]) {
    const char* device = (argc > 1) ? argv[1] : "/dev/video1";

    int fd = open(device, O_RDWR);
    if (fd < 0) { std::cerr << "open " << device << " failed: " << strerror(errno) << '\n'; return 1; }

    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");
    PrintCapability(cap);
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << "device does not support multiplanar capture/streaming\n"; close(fd); return 1;
    }

    auto [width, height, num_planes, stride, fmt] = SetupMultiPlaneFormat(fd);  (void)fmt;

    unsigned buf_count = RequestBuffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, REQ_BUF_COUNT);

    std::vector<MultiPlaneVideoBuffer> buffers(buf_count);
    for (unsigned i = 0; i < buf_count; ++i) {
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i; buf.length = static_cast<__u32>(num_planes); buf.m.planes = mplanes;
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

        buffers[i].planes.resize(num_planes);
        std::cout << "buf[" << i << "]";
        for (int p = 0; p < num_planes; ++p) {
            buffers[i].planes[p].length = mplanes[p].length;
            buffers[i].planes[p].start = mmap(nullptr, mplanes[p].length, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, fd, mplanes[p].m.mem_offset);
            if (buffers[i].planes[p].start == MAP_FAILED)
                throw std::runtime_error(std::string("mmap plane ") + std::to_string(p) + ": " + strerror(errno));
            std::cout << " plane[" << p << "]=" << buffers[i].planes[p].start
                      << " len=" << buffers[i].planes[p].length;
        }
        std::cout << '\n';
    }

    QueueMultiPlaneBuffers(fd, buf_count, num_planes);
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    StartStreaming(fd, type);

    render::OpenGLVideoRenderer renderer;
    if (!InitRenderer(renderer, width, height, "V4L2 MultiPlane + OpenGL Demo", fd, type)) return 1;

    int frame_count = 0;
    std::vector<uint8_t> contiguous_buf;
    std::cout << "capturing... (press ESC to quit)\n";

    while (!renderer.ShouldClose()) {
        PollStatus ps = PollDevice(fd, 100);
        if (ps == PollStatus::Error)         { std::cerr << "poll error\n"; break; }
        if (ps == PollStatus::DeviceError)   { std::cerr << "device error\n"; break; }
        if (ps != PollStatus::Ready)         { renderer.PollEvents(); continue; }

        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
        buf.length = static_cast<__u32>(num_planes); buf.m.planes = mplanes;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) { renderer.PollEvents(); continue; }
            std::cerr << "DQBUF error: " << strerror(errno) << '\n'; break;
        }
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF"); continue;
        }

        size_t total_used = 0;
        for (int p = 0; p < num_planes; ++p)
            total_used += fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
        contiguous_buf.resize(total_used);
        size_t offset = 0;
        for (int p = 0; p < num_planes; ++p) {
            size_t plane_bytes = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
            const uint8_t* src = static_cast<const uint8_t*>(buffers[buf.index].planes[p].start)
                                 + mplanes[p].data_offset;
            memcpy(contiguous_buf.data() + offset, src, plane_bytes);
            offset += plane_bytes;
        }

        auto i420_frame = MediaFrame::CreateCompact(
            contiguous_buf.data(), total_used, PixelFormat::kI420, width, height, stride);

        PrintTimestampClock(buf, frame_count);
        i420_frame->time.pts_us = TimevalToUs(buf.timestamp);

        if (!renderer.Render(*i420_frame)) {
            std::cerr << "render failed: " << renderer.LastError() << '\n';
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF"); break;
        }
        ++frame_count;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
        renderer.PollEvents();
    }

    std::cout << "stopping... rendered " << frame_count << " frames\n";
    StopStreamingAndClose(fd, type);
    return 0;
}

// ================================================================
//  Demo 3: 多平面 YUV420M (DMA-BUF 零拷贝) → OpenGL 渲染
// ================================================================
int v4l2_multiplane_dma_demo(int argc, char* argv[]) {
    const char* device = (argc > 1) ? argv[1] : "/dev/video1";
    const int max_frames = argc > 2 ? std::stoi(argv[2]) : -1;
    const bool strict_import = argc > 3 && std::string(argv[3]) == "--require-import";
    bool failed = false;

    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { std::cerr << "open " << device << " failed: " << strerror(errno) << '\n'; return 1; }

    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");
    PrintCapability(cap);
    auto caps = DeviceCaps(cap);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
        std::cerr << "device does not support multiplanar capture/streaming\n"; close(fd); return 1;
    }

    auto [width, height, num_planes, stride, fmt] = SetupMultiPlaneFormat(fd);  (void)stride;
    if (num_planes != 3) { std::cerr << "YUV420M requires 3 planes\n"; close(fd); return 1; }

    {
        const auto& pf = fmt.fmt.pix_mp.plane_fmt;
        std::cout << "Y: bpl=" << pf[0].bytesperline << " size=" << pf[0].sizeimage
                  << " bpl*h=" << (pf[0].bytesperline * height) << '\n';
        std::cout << "U: bpl=" << pf[1].bytesperline << " size=" << pf[1].sizeimage
                  << " bpl*(h/2)=" << (pf[1].bytesperline * (height / 2))
                  << " Y/2=" << (pf[0].bytesperline / 2) << '\n';
        std::cout << "V: bpl=" << pf[2].bytesperline << " size=" << pf[2].sizeimage
                  << " bpl*(h/2)=" << (pf[2].bytesperline * (height / 2)) << '\n';
        assert(pf[0].bytesperline == (unsigned)width);
        assert(pf[1].bytesperline == pf[0].bytesperline / 2);
        assert(pf[2].bytesperline == pf[0].bytesperline / 2);
        assert(pf[0].sizeimage == pf[0].bytesperline * height);
        assert(pf[1].sizeimage == pf[1].bytesperline * (height / 2));
        assert(pf[2].sizeimage == pf[2].bytesperline * (height / 2));
    }

    unsigned buf_count = RequestBuffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, REQ_BUF_COUNT);

    std::vector<MultiPlaneVideoBuffer> buffers(buf_count);
    std::vector<ExportedBuffer> exported_buffers(buf_count);
    for (unsigned i = 0; i < buf_count; ++i) {
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i; buf.length = static_cast<__u32>(num_planes); buf.m.planes = mplanes;
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

        for (int k = 0; k < num_planes; ++k) {
            v4l2_exportbuffer expbuf{};
            expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; expbuf.index = i;
            expbuf.plane = k; expbuf.flags = O_CLOEXEC | O_RDWR;
            if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                std::cerr << "EXPBUF buf[" << i << "] plane[" << k << "]: " << strerror(errno) << '\n';
                close(fd); return 1;
            }
            exported_buffers[i].dma_fds[k] = expbuf.fd;
        }
        exported_buffers[i].num_planes = num_planes;

        buffers[i].planes.resize(num_planes);
        std::cout << "buf[" << i << "]";
        for (int p = 0; p < num_planes; ++p) {
            buffers[i].planes[p].length = mplanes[p].length;
            std::cout << " plane[" << p << "] fd=" << exported_buffers[i].dma_fds[p]
                      << " len=" << buffers[i].planes[p].length;
        }
        std::cout << '\n';
    }

    QueueMultiPlaneBuffers(fd, buf_count, num_planes);
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    StartStreaming(fd, type);

    render::OpenGLVideoRenderer renderer;
    if (!InitRenderer(renderer, width, height, "V4L2 MultiPlane DMA-BUF + OpenGL Demo", fd, type)) return 1;

    int frame_count = 0;
    std::cout << "capturing... (press ESC to quit)\n";

    while (!renderer.ShouldClose() && (max_frames <= 0 || frame_count < max_frames)) {
        PollStatus ps = PollDevice(fd, 100);
        if (ps == PollStatus::Error)         { failed = true; std::cerr << "poll error\n"; break; }
        if (ps == PollStatus::DeviceError)   { failed = true; std::cerr << "device error\n"; break; }
        if (ps != PollStatus::Ready)         { renderer.PollEvents(); continue; }

        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        if (!DequeueMultiPlaneBuffer(fd, buf, mplanes, num_planes, renderer, failed)) break;

        if (buf.index >= buffers.size() || buf.length != static_cast<unsigned>(num_planes)) {
            std::cerr << "invalid dequeued buffer index or plane count\n"; failed = true; break;
        }

        render::DmaBufVideoFrame dma_frame;
        dma_frame.width = width; dma_frame.height = height; dma_frame.format = PixelFormat::kI420;
        for (int p = 0; p < num_planes; ++p) {
            const auto pitch = fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
            const auto rows = p == 0 ? height : (height + 1) / 2;
            const auto row_bytes = p == 0 ? width : (width + 1) / 2;
            const auto used = static_cast<std::uint64_t>(pitch) * (rows - 1) + row_bytes;
            const uint32_t bytesused = mplanes[p].bytesused
                                       ? mplanes[p].bytesused
                                       : fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
            if (mplanes[p].data_offset > bytesused ||
                bytesused > buffers[buf.index].planes[p].length ||
                used > bytesused - mplanes[p].data_offset ||
                mplanes[p].data_offset > static_cast<unsigned>(std::numeric_limits<int>::max())) {
                failed = true; break;
            }
            dma_frame.planes.push_back({exported_buffers[buf.index].dma_fds[p],
                static_cast<int>(mplanes[p].data_offset), static_cast<int>(pitch),
                buffers[buf.index].planes[p].length});
        }
        if (failed) { std::cerr << "invalid plane payload\n"; break; }

        PrintTimestampClock(buf, frame_count);

        if (!renderer.RenderDmaBuf(dma_frame, !strict_import)) {
            failed = true;
            std::cerr << "render failed: " << renderer.LastError() << '\n';
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF"); break;
        }

        ++frame_count;
        if (frame_count == 1) {
            std::cout << "DMA-BUF render path: "
                      << (renderer.LastDmaBufWasImported() ? "EGLImage import" : "CPU mmap + plane texture upload")
                      << '\n' << renderer.DmaBufImportError() << '\n';
        }

        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
        renderer.PollEvents();
    }

    std::cout << "stopping... rendered " << frame_count << " frames\n";
    StopStreamingAndClose(fd, type);
    renderer.Shutdown();
    return failed ? 1 : 0;
}

int v4l2_multiplane_dma_heap_demo(int argc, char* argv[]) {

    const char *device = argc > 1 ? argv[1] : "/dev/video1";
    const int max_frames = argc > 2 ? std::stoi(argv[2]) : -1;
    const bool strict_import = argc > 3 && std::string(argv[3]) == "--require-import";

    // 打开设备
    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "open " << device << " failed: " << strerror(errno) << '\n';
        return 1;
    }
    // 查询设备能力
    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");
    PrintCapability(cap);
    auto caps = DeviceCaps(cap);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING))
    {
        close(fd);
        return 1;
    }
    // 设置视频格式
    auto [width, height, np, stride, fmt] = SetupMultiPlaneFormat(fd);
    (void)stride;
    if (np != 3 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M)
    {
        close(fd);
        return 1;
    }
    // 打开DMA堆
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0)
    {
        std::cerr << "open dma heap failed: " << strerror(errno) << '\n';
        close(fd);
        return 1;
    }
    // 请求缓冲区
    v4l2_requestbuffers req{};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = REQ_BUF_COUNT;
    xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS(DMABUF)");
    struct HeapBuf
    {
        std::array<int, VIDEO_MAX_PLANES> fd{};
        std::array<size_t, VIDEO_MAX_PLANES> size{};
        ~HeapBuf()
        {
            for (int x : fd)
                if (x >= 0)
                    close(x);
        }
        HeapBuf()
        {
            fd.fill(-1);
            size.fill(0);
        }
        HeapBuf(const HeapBuf &) = delete;
        HeapBuf &operator=(const HeapBuf &) = delete;
    };
    std::vector<HeapBuf> bufs(req.count);
    for (unsigned i = 0; i < req.count; ++i)
        for (int p = 0; p < np; ++p)
        {
            dma_heap_allocation_data a{};
            a.len = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
            a.fd_flags = O_RDWR | O_CLOEXEC;
            if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0)
                throw std::runtime_error("DMA_HEAP_IOCTL_ALLOC: " + std::string(strerror(errno)));
            bufs[i].fd[p] = a.fd;
            bufs[i].size[p] = a.len;
        }
    close(heap);
    auto queue = [&](unsigned i) { 
        v4l2_plane pl[VIDEO_MAX_PLANES]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_DMABUF;
        b.index=i;
        b.length=np;
        b.m.planes=pl;
        for(int p = 0; p < np; ++p) {
            pl[p].m.fd = bufs[i].fd[p];
            pl[p].length = bufs[i].size[p];
        } 
        xioctl(fd,VIDIOC_QBUF,&b,"VIDIOC_QBUF(DMABUF)"); 
    };
    for (unsigned i = 0; i < req.count; ++i) {
        queue(i);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    StartStreaming(fd, type);
    render::OpenGLVideoRenderer renderer;
    if (!InitRenderer(renderer, width, height, "V4L2 DMA-Heap + OpenGL Demo", fd, type))
        return 1;
    int frames = 0;
    bool failed = false;
    std::cout << "capturing... (press ESC to quit)\n";
    while (!renderer.ShouldClose() && (max_frames < 0 || frames < max_frames))
    {
        auto ps = PollDevice(fd, 100);
        if (ps == PollStatus::Error || ps == PollStatus::DeviceError)
        {
            failed = true;
            break;
        }
        if (ps != PollStatus::Ready)
        {
            renderer.PollEvents();
            continue;
        }
        v4l2_plane pl[VIDEO_MAX_PLANES]{};
        v4l2_buffer b{};
        b.type = type;
        b.memory = V4L2_MEMORY_DMABUF;
        b.length = np;
        b.m.planes = pl;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            failed = true;
            break;
        }
        if (b.index >= bufs.size())
        {
            failed = true;
            break;
        }
        if (b.flags & V4L2_BUF_FLAG_ERROR)
        {
            queue(b.index);
            continue;
        }
        render::DmaBufVideoFrame frame;
        frame.width = width;
        frame.height = height;
        frame.format = PixelFormat::kI420;
        for (int p = 0; p < np; ++p)
        {
            if (pl[p].bytesused > bufs[b.index].size[p])
            {
                failed = true;
                break;
            }
            frame.planes.push_back({bufs[b.index].fd[p], 0, (int)fmt.fmt.pix_mp.plane_fmt[p].bytesperline, bufs[b.index].size[p]});
        }
        if (failed)
        {
            queue(b.index);
            break;
        }
        PrintTimestampClock(b, frames);
        if (!renderer.RenderDmaBuf(frame, !strict_import))
        {
            std::cerr << "render failed: " << renderer.LastError() << '\n';
            failed = true;
            queue(b.index);
            break;
        }
        ++frames;
        if (frames == 1)
            std::cout << "DMA-BUF render path: " << (renderer.LastDmaBufWasImported() ? "EGLImage import" : "CPU mmap + plane texture upload") << '\n'
                      << renderer.DmaBufImportError() << '\n';
        queue(b.index);
        renderer.PollEvents();
    }
    std::cout << "stopping... rendered " << frames << " frames\n";
    StopStreamingAndClose(fd, type);
    renderer.Shutdown();
    return failed ? 1 : 0;
}

int main(int argc, char* argv[]) {
    // return v4l2_demo(argc, argv);
    try {
        return v4l2_multiplane_dma_heap_demo(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}