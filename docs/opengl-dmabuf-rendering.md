# OpenGL DMA-BUF rendering

## Entry Point

`render::OpenGLVideoRenderer::RenderDmaBuf(frame, allow_cpu_fallback)` accepts
borrowed DMA-BUF descriptors. Existing `Render(const MediaFrame&)` remains available.
This entry point supports linear I420, NV12, NV21 and GRAY8 image layouts.
It is synchronous and must run on the renderer/context thread.

`render::OpenGLVideoRenderer::RenderDmaBuf(frame, allow_cpu_fallback)` 接受被借用的 DMA-BUF 描述符。现有的 `Render(const MediaFrame&)` 仍然可用。此入口点支持线性 I420、NV12、NV21 和 GRAY8 图像布局。它是同步的，必须在渲染器/上下文线程上运行。

```cpp
render::DmaBufVideoFrame frame;
frame.width = width;
frame.height = height;
frame.format = PixelFormat::kI420; // V4L2_PIX_FMT_YUV420M
for (unsigned p = 0; p < 3; ++p) {
    frame.planes.push_back({
        exported_buffers[buf.index].dma_fds[p],
        static_cast<int>(mplanes[p].data_offset),
        static_cast<int>(fmt.fmt.pix_mp.plane_fmt[p].bytesperline),
        buffers[buf.index].planes[p].length
    });
}
const bool rendered = renderer.RenderDmaBuf(frame);
// Only after RenderDmaBuf returns may the producer overwrite/requeue this buffer.
ioctl(capture_fd, VIDIOC_QBUF, &buf);
```

Check DQBUF index, payload and plane bounds before constructing this descriptor;
the updated `test/v4l2-demo/v4l2.cpp` contains these checks.

- `fd`: one EXPBUF fd for each image plane. A shared fd can occur more than once.
- `offset`: the image-plane start relative to that fd, including `data_offset`.
  Never substitute the V4L2 video-device mmap cookie `m.mem_offset`.
- `pitch`: actual bytesperline for that particular plane, not width or a guessed Y/2.
- `size`: full fd allocation length, including offset/padding, not bytesused.
- `modifier`: currently must be zero (`DRM_FORMAT_MOD_LINEAR`). Tiled/compressed
  allocations and explicit acquire/release fences are not supported.

For single-buffer NV12 or I420, describe each image plane with the same fd and
its actual offset/pitch. Memory-plane count and image-plane count differ.
NV12M has two memory planes, but NV12 may have one memory allocation containing
both Y and UV. NV12 maps to `PixelFormat::kNV12` in either case.

## Paths And Ownership

On Linux, when EGL development support is found, the renderer requests a GLFW
EGL OpenGL context. It falls back to a native context if EGL context creation fails.
EGLImage import requires `EGL_EXT_image_dma_buf_import` and `GL_OES_EGL_image`.
Each plane is imported as R8 or GR88 and sampled through the existing YUV shader.
No CPU image upload occurs on the successful import path. `glFinish()` completes
GPU reads before return, including before buffer requeue. This correctness-first
implementation creates temporary images/textures per call; it is not an asynchronous
fence-based pipeline or a persistent EGLImage cache.

If import is unavailable/rejected, the renderer caches that result for the current
OpenGL context and does not retry EGL import on every frame. This prevents VMware/
Mesa paths from printing a `Failed referencing shared surface` message per frame.
If fallback is allowed, each fd is mapped,
surrounded by `DMA_BUF_IOCTL_SYNC` START/END READ, and uploaded as a separate texture.
This removes the demo's whole-frame packing and CreateCompact copy. It still
uploads CPU memory to the GPU, and an incompatible UV pitch may require row packing;
this path must not be described as GPU zero-copy.

Use `LastDmaBufWasImported()` to inspect the actual path and `DmaBufImportError()`
to inspect why import fell back. `RenderDmaBuf(frame, false)` requires direct import;
it reports failure instead of uploading through CPU memory.

The caller owns all fds and the V4L2 queue. Export failure must not leave garbage
fds; the demo initializes all fd slots to -1 and closes successfully exported fds
through RAII. DQBUF must complete producer writes before rendering, and buffers
must remain dequeued until rendering returns. Keeping an exported fd open does
not prevent the capture device from overwriting a buffer after QBUF.


在 Linux 系统中，当发现 EGL 开发支持时，渲染器会请求一个 GLFWEGL OpenGL 上下文。如果 EGL 上下文创建失败，则回退到原生上下文。EGL 图像导入需要 `EGL_EXT_image_dma_buf_import` 和 `GL_OES_EGL_image`。每个平面以 R8 或 GR88 格式导入，并通过现有的 YUV 着色器进行采样。在成功导入的路径上，不会发生 CPU 图像上传。`glFinish()` 会在返回前完成 GPU 读取操作，包括缓冲区重新排队之前。这种以正确性为优先的实现方式，每次调用都会创建临时图像/纹理；它既不是基于异步栅栏的管道，也不是持久化的 EGLImage 缓存。

如果导入不可用或被拒绝，渲染器会将结果缓存到当前 OpenGL 上下文中，并且不会在每一帧中重试 EGL 导入。这可以防止 VMware/Mesa 路径每帧打印“Failed referencing shared surface”消息。如果允许回退，每个文件描述符（fd）都会被映射，并围绕 `DMA_BUF_IOCTL_SYNC` 的开始/结束读取操作，然后作为独立纹理上传。这消除了演示程序中整帧打包和 CreateCompact 复制的操作。虽然仍然会将 CPU 内存上传到 GPU，但不兼容的 UV 像素间距可能需要行打包处理。此路径不得被描述为GPU零拷贝。

请使用`LastDmaBufWasImported()`来检查实际路径，使用`DmaBufImportError()`来查看导入回退的原因。`RenderDmaBuf(frame, false)`需要直接导入；它会报告失败，而不是通过CPU内存上传。

调用者负责管理所有文件描述符和V4L2队列。导出失败时不应留下未关闭的文件描述符；示例程序通过RAII方式将所有fd槽位初始化为-1，并成功关闭已导出的文件描述符。在渲染前，DQBUF必须完成生产者的写入操作，且缓冲区必须保持脱队状态直到渲染返回。保持已导出的文件描述符打开并不会阻止捕获设备在QBUF后覆盖缓冲区。

## Build And Run

```bash
cd /home/ub22/mxxmstar/video-capture
cmake -S . -B build-v4l2-stage1-asan
cmake --build build-v4l2-stage1-asan --target v4l2_demo test_opengl_dmabuf -j2

# Close the old capture demo first: /dev/video1 must be idle.
# Run from the VM desktop terminal so DISPLAY/XAUTHORITY are already configured.
ASAN_OPTIONS=detect_leaks=0 build-v4l2-stage1-asan/bin/v4l2_demo /dev/video1 120

# Strict direct-import test, without CPU fallback:
ASAN_OPTIONS=detect_leaks=0 build-v4l2-stage1-asan/bin/v4l2_demo /dev/video1 120 --require-import

# Optional hidden-window run:
ASAN_OPTIONS=detect_leaks=0 build-v4l2-stage1-asan/bin/v4l2_demo /dev/video1 30 --hidden

# Pixel tests require access to /dev/dma_heap/system (root on this VM).
sudo --preserve-env=DISPLAY,XAUTHORITY env ASAN_OPTIONS=detect_leaks=0 \
    build-v4l2-stage1-asan/bin/test_opengl_dmabuf

# Also exercise EXPBUF -> render -> QBUF on an idle single-planar vivid node.
sudo --preserve-env=DISPLAY,XAUTHORITY env ASAN_OPTIONS=detect_leaks=0 \
    build-v4l2-stage1-asan/bin/test_opengl_dmabuf /dev/video0
```

Without a frame limit, the demo continues until its window closes. Leak detection
is disabled for these graphics-driver integration runs; address/undefined behavior
instrumentation remains enabled by the existing build configuration.

## Validation On 2026-10-16

The VMware graphics driver rejects the tested system-heap and vivid DMA-BUF imports
with `EGL_BAD_ALLOC` (12291 / 0x3003). Actual rendering uses the CPU upload fallback.
Direct GPU import therefore still requires verification on the target graphics
driver/allocator combination. An EXPBUF fd alone does not guarantee GPU importability.
The first failure is expected on this VM; subsequent frames use the CPU fallback
without repeating the import attempt. `--require-import` remains available to make
this platform limitation a hard test failure.

Pixel tests cover I420/NV12/NV21/GRAY8, aligned and padded layouts, nonzero offsets,
odd UV pitches, shared fds, invalid metadata, strict import behavior, and switching
back to ordinary MediaFrame rendering. The optional vivid test captures 8 frames,
exports buffers, renders three logical planes sharing one fd, and compares rendered
pixels with the source YUV values before requeue. It restores the original format.

The existing user demo on `/dev/video1` was left running. The new three-memory-plane
capture demo compiles, but its runtime test on that occupied node remains pending.
Logs are in `/home/ub22/v4l2-test-env/logs/dmabuf-*.log`.
