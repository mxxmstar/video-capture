#include "render/video/opengl_video_renderer.h"
#include "media/converter/media_frame_converter.h"
#include "media/simple_buffer.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <poll.h>
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

namespace {

void xioctl(int fd, unsigned long request, void* arg, const char* name) {
    if (ioctl(fd, request, arg) == -1) {
        throw std::runtime_error(std::string(name) + ": " + strerror(errno));
    }
}

} // namespace

int v4l2_demo(int argc, char* argv[]) {
    const char* device = (argc > 1) ? argv[1] : "/dev/video0";

    // ================================================================
    //  1. OPEN
    // ================================================================
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        std::cerr << "open " << device << " failed: " << strerror(errno) << std::endl;
        return 1;
    }

    // ================================================================
    //  2. QUERYCAP
    // ================================================================
    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");

    std::cout << "driver:  " << cap.driver  << std::endl;
    std::cout << "card:    " << cap.card    << std::endl;
    std::cout << "bus:     " << cap.bus_info << std::endl;
    std::cout << "capture: " << ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)  ? "YES" : "NO") << std::endl;
    std::cout << "stream:  " << ((cap.capabilities & V4L2_CAP_STREAMING)      ? "YES" : "NO") << std::endl;

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        std::cerr << "device does not support capture" << std::endl;
        close(fd);
        return 1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << "device does not support streaming" << std::endl;
        close(fd);
        return 1;
    }
    // ================================================================
    //  3. S_FMT
    // ================================================================
    v4l2_format fmt{};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // fmt.fmt.pix.width       = 640;
    // fmt.fmt.pix.height      = 480;
    fmt.fmt.pix.width       = 1920;
    fmt.fmt.pix.height      = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");

    // 回读校验
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        std::cout << "driver returned different pixfmt: " << (int)fmt.fmt.pix.pixelformat << std::endl;
    }
    if (fmt.fmt.pix.width  != 640 || fmt.fmt.pix.height != 480) {
        std::cout << "driver adjusted size to " << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height << std::endl;
    }

    int width  = fmt.fmt.pix.width;
    int height = fmt.fmt.pix.height;
    int stride = fmt.fmt.pix.bytesperline;
    std::cout << "format:  " << width << "x" << height
              << "  stride=" << stride << "  size=" << fmt.fmt.pix.sizeimage << std::endl;

    // ================================================================
    //  4. REQBUFS
    // ================================================================
    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = REQ_BUF_COUNT;
    xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");
    std::cout << "buffers: " << req.count << std::endl;

    // ================================================================
    //  5. QUERYBUF + mmap
    // ================================================================
    std::vector<VideoBuffer> buffers(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

        buffers[i].length = buf.length;
        buffers[i].start = mmap(nullptr, buf.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            throw std::runtime_error(std::string("mmap: ") + strerror(errno));
        }
        std::cout << "buf[" << i << "] addr=" << buffers[i].start
                  << " len=" << buffers[i].length << std::endl;
    }

    // ================================================================
    //  6. QBUF（入队，让内核填充数据）
    // ================================================================
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
    }

    // ================================================================
    //  7. STREAMON
    // ================================================================
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
    std::cout << "stream ON" << std::endl;

    // ================================================================
    //  8. OpenGL 渲染器初始化
    // ================================================================
    render::RenderConfig render_config;
    render_config.window_width  = width;
    render_config.window_height = height;
    render_config.title         = "V4L2 Raw + OpenGL Demo";
    render_config.vsync         = false;

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(render_config)) {
        std::cerr << "renderer init failed: " << renderer.LastError() << std::endl;
        xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
        close(fd);
        return 1;
    }

    // YUYV -> I420 converter（OpenGLVideoRenderer 原生支持 I420）
    MediaFrameConverter converter;
    {
        MediaFrameConverterConfig conv_cfg;
        conv_cfg.video = {static_cast<int32_t>(width),
                          static_cast<int32_t>(height),
                          PixelFormat::kI420, 2};
        if (!converter.Open(conv_cfg)) {
            throw std::runtime_error("converter open failed");
        }
    }

    // ================================================================
    //  9. 主循环：DQBUF -> 转换 -> Render -> QBUF
    // ================================================================
    int frame_count = 0;
    std::cout << "capturing... (press ESC to quit)" << std::endl;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;    // 监听读事件

    while (!renderer.ShouldClose()) {
        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;   // 被信号中断，重新轮询
            }
            std::cerr << "poll error: " << strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) {
            renderer.PollEvents();
            continue;   // 超时，重新轮询
        }
        if (pfd.revents & (POLLERR | POLLHUP)) {
            std::cerr << "device error: " << strerror(errno) << std::endl;
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            renderer.PollEvents();
            continue;   // 无数据可读，重新轮询
        }

        // DQBUF —— 取出填好的一帧
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                renderer.PollEvents();
                continue;
            }
            if (errno == EINTR) {
                continue;   // 被信号中断，重新轮询
            }
            std::cerr << "DQBUF error: " << strerror(errno) << std::endl;
            break;
        }

        // 构造 MediaFrame（YUYV 原始数据）
        size_t used = buf.bytesused ? buf.bytesused : fmt.fmt.pix.sizeimage;
        auto yuyv_frame = MediaFrame::CreateCompact(
            static_cast<const unsigned char*>(buffers[buf.index].start), used, PixelFormat::kYUYV422, width, height, stride);

        // 检查并填入时间戳：V4L2 timeval -> 微秒
        if (frame_count == 0) {
            if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
                std::cout << "timestamp clock: CLOCK_MONOTONIC" << std::endl;
            } else if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_COPY) {
                std::cout << "timestamp clock: hardware copy (raw sensor timestamp)" << std::endl;
            } else {
                std::cout << "timestamp clock: CLOCK_REALTIME (may drift with NTP)" << std::endl;
            }
        }
        yuyv_frame->time.pts_us = static_cast<int64_t>(buf.timestamp.tv_sec) * 1'000'000LL
                                + buf.timestamp.tv_usec;

        // YUYV -> I420
        std::shared_ptr<MediaFrame> i420_frame;
        if (!converter.Convert(*yuyv_frame, i420_frame)) {
            std::cerr << "conversion failed at frame " << frame_count << std::endl;
            break;
        }

        // OpenGL 渲染
        if (!renderer.Render(*i420_frame)) {
            std::cerr << "render failed: " << renderer.LastError() << std::endl;
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
            break;
        }

        ++frame_count;
        // if (frame_count % 30 == 0)
        //     std::cout << "frame " << frame_count << std::endl;

        // QBUF —— 归还缓冲区
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");

        renderer.PollEvents();
    }

    // ================================================================
    // 10. 清理
    // ================================================================
    std::cout << "stopping... rendered " << frame_count << " frames" << std::endl;

    xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    close(fd);
}

int v4l2_multiplane_demo(int argc, char* argv[]) {
    const char* device = (argc > 1) ? argv[1] : "/dev/video1";

    // ================================================================
    //  1. OPEN
    // ================================================================
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        std::cerr << "open " << device << " failed: " << strerror(errno) << std::endl;
        return 1;
    }

    // ================================================================
    //  2. QUERYCAP
    // ================================================================
    v4l2_capability cap{};
    xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");

    std::cout << "driver:  " << cap.driver  << std::endl;
    std::cout << "card:    " << cap.card    << std::endl;
    std::cout << "bus:     " << cap.bus_info << std::endl;
    std::cout << "capture: " << ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)  ? "YES" : "NO") << std::endl;
    std::cout << "stream:  " << ((cap.capabilities & V4L2_CAP_STREAMING)      ? "YES" : "NO") << std::endl;

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        std::cerr << "device does not support multiplane capture" << std::endl;
        close(fd);
        return 1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << "device does not support streaming" << std::endl;
        close(fd);
        return 1;
    }

    // ================================================================
    //  3. S_FMT（多平面）
    // ================================================================
    v4l2_format fmt{};
    fmt.type                     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width         = 640;
    fmt.fmt.pix_mp.height        = 480;
    fmt.fmt.pix_mp.pixelformat   = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.field         = V4L2_FIELD_NONE;
    std::cout << "setting format: " << (int)fmt.fmt.pix_mp.pixelformat << std::endl;
    xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");

    // 回读校验
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M) {
        std::cout << "driver returned different pixfmt: " << (int)fmt.fmt.pix_mp.pixelformat << std::endl;
        return 1;
    }
    if (fmt.fmt.pix_mp.width != 640 || fmt.fmt.pix_mp.height != 480) {
        std::cout << "driver adjusted size to " << fmt.fmt.pix_mp.width << "x" << fmt.fmt.pix_mp.height << std::endl;
    }

    int width      = fmt.fmt.pix_mp.width;
    int height     = fmt.fmt.pix_mp.height;
    int num_planes = fmt.fmt.pix_mp.num_planes;   //  多平面核心：驱动返回实际平面数
    int stride     = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    std::cout << "format:  " << width << "x" << height
              << "  planes=" << num_planes
              << "  stride=" << stride
              << "  size[0]=" << fmt.fmt.pix_mp.plane_fmt[0].sizeimage << std::endl;

    for (int p = 0; p < num_planes; ++p) {
    std::cout << "  plane[" << p << "]"
              << " bytesperline=" << fmt.fmt.pix_mp.plane_fmt[p].bytesperline
              << " sizeimage="    << fmt.fmt.pix_mp.plane_fmt[p].sizeimage
              << std::endl;
    
    const auto& pf = fmt.fmt.pix_mp.plane_fmt;
    std::cout << "Y:  bpl=" << pf[0].bytesperline
            << " size=" << pf[0].sizeimage
            << " bpl*h=" << (pf[0].bytesperline * height)
            << std::endl;
    std::cout << "U:  bpl=" << pf[1].bytesperline
            << " size=" << pf[1].sizeimage
            << " bpl*(h/2)=" << (pf[1].bytesperline * (height/2))
            << " Y/2=" << (pf[0].bytesperline / 2)
            << std::endl;
    std::cout << "V:  bpl=" << pf[2].bytesperline
            << " size=" << pf[2].sizeimage
            << " bpl*(h/2)=" << (pf[2].bytesperline * (height/2))
            << std::endl;
}
    // ================================================================
    //  4. REQBUFS
    // ================================================================
    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = REQ_BUF_COUNT;
    xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");
    std::cout << "buffers: " << req.count << std::endl;

    // ================================================================
    //  5. QUERYBUF + mmap（每个平面独立映射）
    // ================================================================
    std::vector<MultiPlaneVideoBuffer> buffers(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = static_cast<__u32>(num_planes);  // 平面数量
        buf.m.planes = mplanes;                          // 指向平面数组
        xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");        

        buffers[i].planes.resize(num_planes);
        std::cout << "buf[" << i << "]";
        for (int p = 0; p < num_planes; ++p) {
            buffers[i].planes[p].length = mplanes[p].length;
            buffers[i].planes[p].start = mmap(nullptr, mplanes[p].length,
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       fd, mplanes[p].m.mem_offset);
            if (buffers[i].planes[p].start == MAP_FAILED) {
                throw std::runtime_error(std::string("mmap plane ") + std::to_string(p) + ": " + strerror(errno));
            }
            std::cout << " plane[" << p << "]=" << buffers[i].planes[p].start
                      << " len=" << buffers[i].planes[p].length;
        }
        std::cout << std::endl;
    }

    // ================================================================
    //  6. QBUF（入队，让内核填充数据）
    // ================================================================
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = static_cast<__u32>(num_planes);
        buf.m.planes = mplanes;
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
        if (buf.length != static_cast<__u32>(num_planes)) {
            std::cerr << "unexpected plane count: " << buf.length << std::endl;
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
            continue;
        }
        
    }

    // ================================================================
    //  7. STREAMON
    // ================================================================
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
    std::cout << "stream ON" << std::endl;

    // ================================================================
    //  8. OpenGL 渲染器初始化
    // ================================================================
    render::RenderConfig render_config;
    render_config.window_width  = width;
    render_config.window_height = height;
    render_config.title         = "V4L2 MultiPlane + OpenGL Demo";
    render_config.vsync         = false;

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(render_config)) {
        std::cerr << "renderer init failed: " << renderer.LastError() << std::endl;
        xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
        close(fd);
        return 1;
    }

    // YUV420M 数据已经是 I420 格式，OpenGL 渲染器原生支持，无需转换
    // ================================================================
    //  8. OpenGL 渲染器初始化
    // ================================================================
    int frame_count = 0;
    std::cout << "capturing... (press ESC to quit)" << std::endl;
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLIN;

    // 用于拼合多平面数据的临时缓冲区
    std::vector<uint8_t> contiguous_buf;

    while (!renderer.ShouldClose()) {
        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll error: " << strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) {
            renderer.PollEvents();
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) {
            std::cerr << "device error: " << strerror(errno) << std::endl;
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            renderer.PollEvents();
            continue;
        }

        // DQBUF —— 取出填好的一帧（多平面）
        v4l2_plane mplanes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.length   = static_cast<__u32>(num_planes);
        buf.m.planes = mplanes;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                renderer.PollEvents();
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "DQBUF error: " << strerror(errno) << std::endl;
            break;
        }

        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");  // 直接归还，丢弃这帧
            continue;
        }

        // 拼合多平面数据到连续缓冲区
        // 注意：用 plane_fmt[].sizeimage 而非 bytesused，
        // 保证每个平面的 stride padding 不被截断，
        // 这样 CreateCompact 内部的 SourcePlaneLayout::Compute
        // 按 stride*rows 计算的各平面偏移才能对齐
        size_t total_used = 0;
        for (int p = 0; p < num_planes; ++p) {
            total_used += fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
        }
        contiguous_buf.resize(total_used);
        size_t offset = 0;
        for (int p = 0; p < num_planes; ++p) {
            size_t plane_bytes = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
            const uint8_t* src = static_cast<const uint8_t*>(buffers[buf.index].planes[p].start)
                   + mplanes[p].data_offset; // data_offset 不一定为 0
            memcpy(contiguous_buf.data() + offset, src, plane_bytes);            
            offset += plane_bytes;
        }

        // 构造 MediaFrame（YUV420M -> I420，格式天然兼容）
        auto i420_frame = MediaFrame::CreateCompact(
            contiguous_buf.data(), total_used,
            PixelFormat::kI420, width, height, stride);

        // 检查并填入时间戳：V4L2 timeval -> 微秒
        if (frame_count == 0) {
            if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
                std::cout << "timestamp clock: CLOCK_MONOTONIC" << std::endl;
            } else if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_COPY) {
                std::cout << "timestamp clock: hardware copy (raw sensor timestamp)" << std::endl;
            } else {
                std::cout << "timestamp clock: CLOCK_REALTIME (may drift with NTP)" << std::endl;
            }
        }
        i420_frame->time.pts_us = static_cast<int64_t>(buf.timestamp.tv_sec) * 1'000'000LL
                                + buf.timestamp.tv_usec;

        // OpenGL 渲染（YUV420M 即 I420，渲染器原生支持，零转换）
        if (!renderer.Render(*i420_frame)) {
            std::cerr << "render failed: " << renderer.LastError() << std::endl;
            xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
            break;
        }

        ++frame_count;

        // QBUF —— 归还缓冲区
        xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");

        renderer.PollEvents();
    }

    // ================================================================
    // 10. 清理
    // ================================================================
    std::cout << "stopping... rendered " << frame_count << " frames" << std::endl;

    xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    close(fd);
}

int main(int argc, char* argv[]) {
    // return v4l2_demo(argc, argv);
    return v4l2_multiplane_demo(argc, argv);

}