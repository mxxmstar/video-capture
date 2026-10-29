#include "render/video/opengl_video_renderer.h"
#include <glad/glad.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Allocation {
        int fd{-1};
        unsigned char *data{nullptr};
        ~Allocation() {
            if (data) {
                munmap(data, 4096);
            }
            if (fd >= 0) {
                close(fd);
            }
        }
        void Allocate(int heap) {
            dma_heap_allocation_data request{};
            request.len = 4096;
            request.fd_flags = O_RDWR | O_CLOEXEC;
            if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request) < 0)
                throw std::runtime_error("heap allocation failed");
            fd = request.fd;
            void *ptr = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED)
                throw std::runtime_error("heap mmap failed");
            data = static_cast<unsigned char *>(ptr);
        }
        void Sync(unsigned long long flags) {
            dma_buf_sync request{};
            request.flags = flags;
            if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &request) < 0)
                throw std::runtime_error("heap sync failed");
        }
    };
    void Require(bool ok, const char *text) {
        if (!ok) {
            throw std::runtime_error(text);
        }
    }

    // Optional integration check on an idle single-planar vivid node. Restore its format.
    void CaptureSmoke(render::OpenGLVideoRenderer &renderer, const char *device)
    {
        struct Capture {
            int fd{-1}, exported{-1};
            bool streaming{false}, restore{false};
            v4l2_format original{};
            ~Capture() {
                if (fd < 0) {
                    return;
                }
                auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (streaming) {
                    ioctl(fd, VIDIOC_STREAMOFF, &type);
                }
                if (exported >= 0) {
                    close(exported);
                }
                v4l2_requestbuffers req{};
                req.type = type;
                req.memory = V4L2_MEMORY_MMAP;
                ioctl(fd, VIDIOC_REQBUFS, &req);
                if (restore) {
                    ioctl(fd, VIDIOC_S_FMT, &original);
                }
                close(fd);
            }
        } capture;

        // 打开设备文件
        capture.fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        Require(capture.fd >= 0, "open vivid failed");

        // 查询当前格式
        capture.original.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        Require(ioctl(capture.fd, VIDIOC_G_FMT, &capture.original) == 0, "query vivid format failed");
        auto fmt = capture.original;
        fmt.fmt.pix.width = 640;
        fmt.fmt.pix.height = 480;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        Require(ioctl(capture.fd, VIDIOC_S_FMT, &fmt) == 0, "configure idle single-planar vivid failed");
        capture.restore = true;
        Require(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420, "vivid rejected I420");

        // 请求缓冲区
        v4l2_requestbuffers req{};
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        req.count = 2;
        Require(ioctl(capture.fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 2, "vivid REQBUFS failed");
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = req.type;
            buf.memory = req.memory;
            buf.index = i;
            Require(ioctl(capture.fd, VIDIOC_QBUF, &buf) == 0, "vivid QBUF failed");
        }

        // 开启流
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        Require(ioctl(capture.fd, VIDIOC_STREAMON, &type) == 0, "vivid STREAMON failed");
        capture.streaming = true;
        for (int i = 0; i < 8; ++i) {
            pollfd pfd{capture.fd, POLLIN, 0};
            Require(poll(&pfd, 1, 3000) > 0, "vivid capture timed out");
            
            // 从队列中获取缓冲区
            v4l2_buffer buf{};
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            Require(ioctl(capture.fd, VIDIOC_DQBUF, &buf) == 0, "vivid DQBUF failed");
            
            // 导出缓冲区
            v4l2_exportbuffer exp{};
            exp.type = type;
            exp.index = buf.index;
            exp.flags = O_RDWR | O_CLOEXEC;
            Require(ioctl(capture.fd, VIDIOC_EXPBUF, &exp) == 0, "vivid EXPBUF failed");
            capture.exported = exp.fd;

            // 渲染视频帧
            render::DmaBufVideoFrame frame;
            frame.width = fmt.fmt.pix.width;
            frame.height = fmt.fmt.pix.height;
            frame.format = PixelFormat::kI420;
            const int pitch = fmt.fmt.pix.bytesperline;
            const int ysize = pitch * frame.height;
            const int csize = (pitch / 2) * ((frame.height + 1) / 2);
            frame.planes = {{exp.fd, 0, pitch, buf.length},
                            {exp.fd, ysize, pitch / 2, buf.length},
                            {exp.fd, ysize + csize, pitch / 2, buf.length}};
            if (!renderer.RenderDmaBuf(frame)) {
                throw std::runtime_error(renderer.LastError());
            }

            // DMA-BUF 渲染后像素回读校验
            void *source = mmap(nullptr, buf.length, PROT_READ, MAP_SHARED, exp.fd, 0);
            Require(source != MAP_FAILED, "map captured frame for pixel check failed");
            
            // cpu 同步
            dma_buf_sync sync{};
            sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
            const bool sync_ok = ioctl(exp.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
            bool pixels_ok = sync_ok;
            // 确保dma写入对cpu可见
            if (sync_ok) {
                const auto *data = static_cast<const unsigned char *>(source);
                for (int sample : {3, 8, 12}) {
                    const float u = (sample + 0.5f) / 16, v = 1 - u;
                    const auto linear = [&](int offset, int stride, int w, int h) {
                        const float x = std::clamp(u * w - 0.5f, 0.0f, float(w - 1));
                        const float y = std::clamp(v * h - 0.5f, 0.0f, float(h - 1));
                        const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                        const float fx = x - x0, fy = y - y0;
                        return (1 - fy) * ((1 - fx) * data[offset + y0 * stride + x0] + fx * data[offset + y0 * stride + x1]) + fy * ((1 - fx) * data[offset + y1 * stride + x0] + fx * data[offset + y1 * stride + x1]);
                    };
                    const float y = (linear(0, pitch, frame.width, frame.height) - 16) * 298 / 256;
                    const float cb = linear(ysize, pitch / 2, frame.width / 2, frame.height / 2) - 128;
                    const float cr = linear(ysize + csize, pitch / 2, frame.width / 2, frame.height / 2) - 128;
                    const std::array<float, 3> expected{y + 409 * cr / 256, y - 100 * cb / 256 - 208 * cr / 256, y + 516 * cb / 256};
                    std::array<unsigned char, 4> pixel{};
                    glReadPixels(sample, sample, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
                    for (int channel = 0; channel < 3; ++channel) {
                        if (std::abs(int(pixel[channel]) - std::clamp(expected[channel], 0.0f, 255.0f)) > 4) {
                            pixels_ok = false;
                        }
                    }
                }
                // 通知 dma 引擎 cpu 读取完成
                sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                if (ioctl(exp.fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
                    pixels_ok = false;
                }
            }
            munmap(source, buf.length);
            Require(pixels_ok, "captured DMA-BUF pixels differ from source YUV reference");
            if (i == 0)
                std::cout << "vivid path=" << (renderer.LastDmaBufWasImported() ? "EGLImage" : "CPU upload")
                          << " reason=" << renderer.DmaBufImportError() << '\n';
            close(capture.exported);
            capture.exported = -1;
            Require(ioctl(capture.fd, VIDIOC_QBUF, &buf) == 0, "vivid requeue failed");
        }
        std::cout << "PASS vivid EXPBUF -> RenderDmaBuf -> QBUF: 8 frames, three image planes sharing one fd\n";
    }
}

int main(int argc, char **argv)
{
    const int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0)
    {
        std::cerr << "SKIP: /dev/dma_heap/system requires access\n";
        return 77;
    }
    try
    {
        std::array<Allocation, 3> buffers;
        for (auto &buffer : buffers)
            buffer.Allocate(heap);
        close(heap);
        render::OpenGLVideoRenderer renderer;
        render::RenderConfig config;
        config.window_width = config.window_height = 16;
        config.visible = false;
        config.vsync = false;
        if (!renderer.Init(config))
            throw std::runtime_error(renderer.LastError());
        GLuint fbo = 0, color = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "FBO incomplete");
        for (auto format : {PixelFormat::kI420, PixelFormat::kNV12, PixelFormat::kNV21, PixelFormat::kGRAY8})
        {
            for (bool padded : {false, true})
            {
                const int count = format == PixelFormat::kI420 ? 3 : format == PixelFormat::kGRAY8 ? 1 : 2;
                render::DmaBufVideoFrame frame;
                frame.width = frame.height = 8;
                frame.format = format;
                for (int i = 0; i < count; ++i)
                {
                    auto &buffer = buffers[i];
                    buffer.Sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                    std::memset(buffer.data, 0xee, 4096);
                    const bool uv = i == 1 && count == 2;
                    const int rows = i == 0 ? 8 : 4;
                    const int columns = i == 0 || uv ? 8 : 4;
                    const int pitch = padded ? (i == 0 ? 20 : 13) : 64;
                    const int offset = padded ? 17 : 0;
                    for (int y = 0; y < rows; ++y)
                    {
                        for (int x = 0; x < columns; ++x)
                        {
                            const int value = i == 0   ? (format == PixelFormat::kGRAY8 ? 128 : 81)
                                              : uv     ? (((x % 2 == 0) == (format == PixelFormat::kNV12)) ? 90 : 240)
                                              : i == 1 ? 90
                                                       : 240;
                            buffer.data[offset + y * pitch + x] = value;
                        }
                    }
                    buffer.Sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    frame.planes.push_back({buffer.fd, offset, pitch, 4096});
                }
                if (padded && format == PixelFormat::kNV21)
                {
                    buffers[0].Sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                    buffers[1].Sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                    std::memcpy(buffers[0].data + 512, buffers[1].data + 17, 4 * 13);
                    buffers[1].Sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                    buffers[0].Sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    frame.planes[1].fd = buffers[0].fd;
                    frame.planes[1].offset = 512;
                }
                for (int repeat = 0; repeat < 3; ++repeat)
                {
                    if (!renderer.RenderDmaBuf(frame))
                        throw std::runtime_error(renderer.LastError());
                    std::array<unsigned char, 4> pixel{};
                    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
                    const auto expected = format == PixelFormat::kGRAY8 ? std::array<int, 3>{128, 128, 128}
                                                                        : std::array<int, 3>{255, 0, 0};
                    for (int c = 0; c < 3; ++c)
                        Require(std::abs(int(pixel[c]) - expected[c]) <= 3, "DMA-BUF pixel mismatch");
                }
                std::cout << "PASS format=" << int(format) << " padded=" << padded << " path="
                          << (renderer.LastDmaBufWasImported() ? "EGLImage" : "CPU upload")
                          << " reason=" << renderer.DmaBufImportError() << '\n';
                auto invalid = frame;
                invalid.planes[0].offset = 4096;
                Require(!renderer.RenderDmaBuf(invalid), "invalid allocation bounds accepted");
                invalid = frame;
                invalid.planes[0].fd = -1;
                Require(!renderer.RenderDmaBuf(invalid), "invalid fd accepted");
                invalid = frame;
                invalid.planes.pop_back();
                Require(!renderer.RenderDmaBuf(invalid), "invalid plane count accepted");
                const bool strict = renderer.RenderDmaBuf(frame, false);
                Require(strict == renderer.LastDmaBufWasImported(), "strict mode silently used fallback");
                if (!strict)
                    Require(!renderer.LastError().empty(), "missing strict import error");
            }
        }
        if (argc > 1)
            CaptureSmoke(renderer, argv[1]);
        // Switching back to the existing CPU MediaFrame API must still work.
        std::array<unsigned char, 16 * 16 * 3 / 2> regular_pixels{};
        std::fill_n(regular_pixels.begin(), 256, 81);
        std::fill_n(regular_pixels.begin() + 256, 64, 90);
        std::fill_n(regular_pixels.begin() + 320, 64, 240);
        auto regular = MediaFrame::CreateCompact(regular_pixels.data(), regular_pixels.size(), PixelFormat::kI420, 16, 16, 16);
        Require(regular != nullptr, "regular frame construction failed");
        if (!renderer.Render(*regular))
            throw std::runtime_error(renderer.LastError());
        std::array<unsigned char, 4> gray_pixel{};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, gray_pixel.data());
        Require(gray_pixel[0] >= 252 && gray_pixel[1] <= 3 && gray_pixel[2] <= 3, "ordinary MediaFrame rendering regressed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
        renderer.Shutdown();
        std::cout << "PASS DMA-BUF pixel/layout/strict-import tests\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
