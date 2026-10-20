#include "render/video/opengl_video_renderer.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#ifdef RENDER_EGL_DMABUF
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

namespace render {
namespace {
struct PlaneLayout { int width; int height; int channels; };
#ifdef __linux__
int SyncCpu(int fd, unsigned long long flags) {
    dma_buf_sync sync{};
    sync.flags = flags;
    int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (rc < 0 && errno == EINTR);
    return rc;
}
#endif
#ifdef RENDER_EGL_DMABUF
bool HasExtension(const char* extensions, const char* name) {
    if (!extensions) return false;
    const std::string list = std::string(" ") + extensions + " ";
    return list.find(std::string(" ") + name + " ") != std::string::npos;
}
#endif
}

bool OpenGLVideoRenderer::RenderDmaBuf(const DmaBufVideoFrame& frame, bool allow_cpu_fallback) {
    // 状态重置与前置检查
    last_dmabuf_imported_ = false;
    if (!dmabuf_import_disabled_) {
        dmabuf_import_error_.clear();
    }
    if (!initialized_ && !Init(config_)) {
        return false;
    }
    if (!window_ || ShouldClose()) {
        return false;
    }
    glfwMakeContextCurrent(window_);

    // DRM_FORMAT_MOD_LINEAR 仅支持线性布局，不支持 tiled/compressed modifier
    if (frame.modifier != 0) {
        setError("DMA-BUF tiled/compressed modifiers are not supported"); return false;
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 32768 || frame.height > 32768) {
        setError("invalid DMA-BUF dimensions"); return false;
    }

    // 格式 -> 平面布局和 shader 参数推导
    // shader_format 映射: 1=NV12, 2=NV21, 3=I420, 5=GRAY8
    int shader_format = 0;
    int count = 0;
    std::array<PlaneLayout, 3> layout{};     // {width, height, channels}
    layout[0] = {frame.width, frame.height, 1};
    const int cw = (frame.width + 1) / 2, ch = (frame.height + 1) / 2;
    switch (frame.format) {
    case PixelFormat::kI420:
        count = 3; shader_format = 3;
        layout[1] = layout[2] = {cw, ch, 1}; break;   // U 和 V 各占 1/4 分辨率，单通道
    case PixelFormat::kNV12:
    case PixelFormat::kNV21:
        count = 2; shader_format = frame.format == PixelFormat::kNV12 ? 1 : 2;
        layout[1] = {cw, ch, 2}; break;               // UV 交错，2 通道
    case PixelFormat::kGRAY8:
        count = 1; shader_format = 5; break;
    default: setError("unsupported DMA-BUF format (expected linear I420/NV12/NV21/GRAY8)"); return false;
    }
    if (frame.planes.size() != static_cast<std::size_t>(count)) {
        setError("DMA-BUF image plane count mismatch"); return false;
    }

    // 逐平面参数校验
    int max_texture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    for (int i = 0; i < count; ++i) {
        const auto& p = frame.planes[i];
        const auto& l = layout[i];
        // 该平面最后一行最后一个有效像素在 fd 内的字节偏移
        const auto bytes = static_cast<std::uint64_t>(p.pitch > 0 ? p.pitch : 0) * (l.height - 1)
                         + l.width * l.channels;
        if (p.fd < 0 || p.offset < 0 || p.pitch < l.width * l.channels ||
            p.size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            static_cast<std::size_t>(p.offset) > p.size || bytes > p.size - p.offset ||
            l.width > max_texture || l.height > max_texture) {
            setError("invalid DMA-BUF fd, offset, pitch or allocation bounds"); return false;
        }
#ifdef __linux__
        // 用 fstat 进一步验证 fd 有效性及分配大小
        struct stat st{};
        if (fstat(p.fd, &st) < 0 || (st.st_size > 0 && static_cast<std::uint64_t>(st.st_size) < p.size)) {
            setError("DMA-BUF fd is closed or allocation size exceeds fd size"); return false;
        }
#endif
    }

    // EGL DMA-BUF 零拷贝 GPU 导入（首选路径）
    // 通过 eglCreateImageKHR -> glEGLImageTargetTexture2DOES 将 dma-buf fd
    // 直接映射为 GL 纹理，无需 CPU 端 mmap 拷贝。仅在编译期开启 RENDER_EGL_DMABUF、
    // 运行时 EGL 扩展可用、且之前未标记禁用时尝试。
#ifdef RENDER_EGL_DMABUF
    if (dmabuf_import_disabled_) {
        if (!allow_cpu_fallback) {
            setError(dmabuf_import_error_); return false;
        }
    } else {
        const EGLDisplay display = eglGetCurrentDisplay();
        auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        using BindImage = void (*)(GLenum, void*);
        auto bind_image = reinterpret_cast<BindImage>(glfwGetProcAddress("glEGLImageTargetTexture2DOES"));
        // 检测 GL_OES_EGL_image 扩展
        bool gl_image = false;
        GLint extensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; i < extensions; ++i) {
            if (std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)), "GL_OES_EGL_image") == 0) {
                gl_image = true;
            }
        }
        if (display != EGL_NO_DISPLAY && create_image && destroy_image && bind_image && gl_image &&
            HasExtension(eglQueryString(display, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import")) {
            std::array<EGLImageKHR, 3> images{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
            std::array<GLuint, 3> textures{};
            bool imported = true;
            glActiveTexture(GL_TEXTURE0);
            for (int i = 0; i < count; ++i) {
                const auto& p = frame.planes[i];
                const auto& l = layout[i];
                // DRM_FORMAT_R8 = 0x20203852, DRM_FORMAT_GR88 = 0x38385247
                // 单通道 -> R8，双通道(NV12/NV21 UV) -> GR88，字节序与 GL RED/RG 一致
                const EGLint fourcc = l.channels == 1 ? 0x20203852 : 0x38385247;
                const EGLint attrs[] = {
                    EGL_WIDTH, l.width, EGL_HEIGHT, l.height, EGL_LINUX_DRM_FOURCC_EXT, fourcc,
                    EGL_DMA_BUF_PLANE0_FD_EXT, p.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, p.offset,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, p.pitch, EGL_NONE};

                // 创建 EGLImage，将 dma-buf 导入 EGL
                images[i] = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
                if (images[i] == EGL_NO_IMAGE_KHR) {
                    dmabuf_import_error_ = "eglCreateImageKHR failed: " + std::to_string(eglGetError());
                    // 导入结果与具体 EGL context/allocator 相关，失败后不再逐帧重试：
                    // 某些 Mesa/VMware 路径每次尝试都会打印内核级 SID 错误日志
                    dmabuf_import_disabled_ = true;
                    imported = false; break;
                }

                // 为导入的 EGLImage 创建 GL 纹理并绑定
                glGenTextures(1, &textures[i]);
                glBindTexture(GL_TEXTURE_2D, textures[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                bind_image(GL_TEXTURE_2D, images[i]);   // 将 EGLImage 绑定为目标 2D 纹理
                if (glGetError() != GL_NO_ERROR) {
                    dmabuf_import_error_ = "glEGLImageTargetTexture2DOES rejected imported plane";
                    dmabuf_import_disabled_ = true;
                    imported = false; break;
                }
            }

            // 全部平面导入成功后，使用临时纹理指针调用 shader 绘制
            bool drawn = false;
            if (imported) {
                const std::array<GLuint, 3> saved{texture_y_, texture_plane1_, texture_plane2_};
                texture_y_ = textures[0]; texture_plane1_ = textures[1]; texture_plane2_ = textures[2];
                drawn = drawVideoTextures(shader_format);
                glFinish();    // 同步等待 GPU 完成绘制：返回后调用方可安全 QBUF/关闭 fd
                const auto error = glGetError();
                if (error != GL_NO_ERROR) { setError("DMA-BUF draw failed: " + std::to_string(error)); drawn = false; }
                texture_y_ = saved[0]; texture_plane1_ = saved[1]; texture_plane2_ = saved[2];
            }

            // 清理本次创建的临时纹理和 EGLImage
            glDeleteTextures(3, textures.data());
            for (auto image : images) if (image != EGL_NO_IMAGE_KHR) destroy_image(display, image);
            if (imported) { last_dmabuf_imported_ = drawn; return drawn; }
        } else {
            // EGL 扩展不可用，标记禁用并走回退路径
            dmabuf_import_error_ = "EGL DMA-BUF import / GL_OES_EGL_image unavailable on this context";
            dmabuf_import_disabled_ = true;
        }
    }
#else
    dmabuf_import_error_ = "renderer built without EGL DMA-BUF import";
#endif

    // CPU mmap 回退路径
    // 当 EGL DMA-BUF 导入不可用或不被允许时，退化为：
    // mmap -> DMA_BUF_SYNC_START（确保生产者写入对 CPU 可见）-> uploadPlane（上传到 GL 纹理）
    // -> DMA_BUF_SYNC_END（通知 DMA 引擎 CPU 读取完成）-> munmap -> shader 绘制
    if (!allow_cpu_fallback) { setError(dmabuf_import_error_); return false; }
#ifdef __linux__
    // 指向各平面复用纹理的指针数组，uploadPlane 会按需更新纹理尺寸和格式
    std::array<std::uint32_t*, 3> textures{&texture_y_, &texture_plane1_, &texture_plane2_};
    std::array<int*, 3> widths{&texture_y_width_, &texture_plane1_width_, &texture_plane2_width_};
    std::array<int*, 3> heights{&texture_y_height_, &texture_plane1_height_, &texture_plane2_height_};
    std::array<unsigned int*, 3> formats{&texture_y_internal_format_, &texture_plane1_internal_format_, &texture_plane2_internal_format_};
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < count; ++i) {
        const auto& p = frame.planes[i];
        const auto& l = layout[i];

        // mmap 将 dma-buf 映射到用户态地址空间
        void* mapping = mmap(nullptr, p.size, PROT_READ, MAP_SHARED, p.fd, 0);
        if (mapping == MAP_FAILED) {
            setError(std::string("DMA-BUF mmap: ") + std::strerror(errno));
            return false;
        }

        // DMA_BUF_SYNC_START: 确保 GPU/硬件写入对 CPU 可见后才读取
        if (SyncCpu(p.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) {
            const std::string error = std::strerror(errno);
            munmap(mapping, p.size); setError("DMA-BUF CPU sync start: " + error);
            return false;
        }

        // 上传该平面到 GL 纹理（哈 pixel_format=RED/RG, internal_format=R8/RG8）
        const bool uploaded = uploadPlane(*textures[i], *widths[i], *heights[i], *formats[i],
            l.width, l.height, l.width * l.channels, p.pitch,
            static_cast<const std::uint8_t*>(mapping) + p.offset,
            l.channels == 1 ? GL_RED : GL_RG, l.channels == 1 ? GL_R8 : GL_RG8, "DMA-BUF plane");

        // DMA_BUF_SYNC_END: 通知 DMA 引擎 CPU 已完成读取，允许后续写入
        const bool synced = SyncCpu(p.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) == 0;
        munmap(mapping, p.size);
        if (!synced) {
            setError("DMA-BUF CPU sync end failed");
            return false;
        }
        if (!uploaded) {
            return false;
        }
    }

    // 所有平面上传完成后，通过 YUV shader 绘制到全屏 quad
    const bool drawn = drawVideoTextures(shader_format);
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        setError("DMA-BUF fallback draw failed: " + std::to_string(error));
        return false;
    }
    return drawn;
#else
    setError("DMA-BUF rendering requires Linux"); return false;
#endif
}
} // namespace render