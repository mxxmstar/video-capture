#include "media/capture/v4l2_video_capture.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
/// @brief 执行 ioctl 操作，处理 EINTR 错误
/// @param fd 文件描述符
/// @param request ioctl 请求码
/// @param argument 请求参数
/// @return ioctl 返回值
int Ioctl(int fd, unsigned long request, void* argument) {
    int result;
    do { result = ::ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result;
}

/// @brief 将 PixelFormat 转换为 V4L2_PIX_FMT 格式
/// @param format 原始像素格式
/// @return 对应的 V4L2_PIX_FMT 格式，0 表示未知
unsigned int Fourcc(PixelFormat format) {
    switch (format) {
    case PixelFormat::kYUYV422: return V4L2_PIX_FMT_YUYV;
    case PixelFormat::kNV12: return V4L2_PIX_FMT_NV12;
    case PixelFormat::kI420: return V4L2_PIX_FMT_YUV420;
    default: return 0;
    }
}

PixelFormat Pixel(unsigned int format) {
    switch (format) {
    case V4L2_PIX_FMT_YUYV: return PixelFormat::kYUYV422;
    case V4L2_PIX_FMT_NV12: return PixelFormat::kNV12;
    case V4L2_PIX_FMT_YUV420: return PixelFormat::kI420;
    default: return PixelFormat::kUnknown;
    }
}

CaptureError NativeError(const char* operation, int code) {
    auto category = CaptureErrorCategory::System;
    bool retryable = false;
    switch (code) {
    case ENOENT: category = CaptureErrorCategory::NotFound; retryable = true; break;
    case EACCES: case EPERM: category = CaptureErrorCategory::PermissionDenied; break;
    case EBUSY: category = CaptureErrorCategory::Busy; retryable = true; break;
    case ENODEV: case ENXIO: category = CaptureErrorCategory::DeviceDisconnected; retryable = true; break;
    case EINVAL: case ENOTTY: category = CaptureErrorCategory::Unsupported; break;
    case EIO: retryable = true; break;
    }
    return {category, code, std::string(operation) + ": " + std::strerror(code), retryable};
}

CaptureReadResult ReadFailure(CaptureError error) {
    const auto status = error.retryable ? CaptureReadStatus::RetryableError : CaptureReadStatus::FatalError;
    return {status, nullptr, std::move(error)};
}
}

struct V4L2VideoCapture::BufferPool {
    struct Mapping { void* address; std::size_t length; };
    int fd{-1};
    bool active{false};
    std::mutex mutex;
    std::vector<Mapping> mappings;

    ~BufferPool() {
        for (const auto& mapping : mappings) ::munmap(mapping.address, mapping.length);
        if (fd >= 0) ::close(fd);
    }

    void Return(v4l2_buffer buffer) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (active && Ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("Deferred capture QBUF failed: {}", std::strerror(errno));
        }
    }

    void Stop() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (active) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            Ioctl(fd, VIDIOC_STREAMOFF, &type);
            active = false;
        }
        // Detach buffers from the queue; supported drivers orphan retained mappings.
        if (fd >= 0) {
            v4l2_requestbuffers request{};
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            if (Ioctl(fd, VIDIOC_REQBUFS, &request) < 0 && errno != EINVAL) {
                LOG_WARN("Cannot release capture queue with retained mappings: {}", std::strerror(errno));
            }
        }
        // Retained frames keep mappings alive, but cannot requeue after shutdown.
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

struct DeferredBufferReturn {
    std::function<void()> return_buffer;
    ~DeferredBufferReturn() { if (return_buffer) return_buffer(); }
};

V4L2VideoCapture::V4L2VideoCapture(V4L2CaptureConfig config) : config_(std::move(config)) {}
V4L2VideoCapture::~V4L2VideoCapture() { Close(); }

CaptureOpenResult V4L2VideoCapture::Open() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    cleanup();
    stop_requested_.store(false);
    const auto failed = [this](CaptureError error) {
        cleanup();
        stop_requested_.store(true);
        return CaptureOpenResult{std::move(error)};
    };
    if (config_.device.empty() || config_.width <= 0 || config_.height <= 0 ||
        config_.width > 16384 || config_.height > 16384 || config_.width % 2 != 0 ||
        (config_.pixel_format != PixelFormat::kYUYV422 && config_.height % 2 != 0) ||
        Fourcc(config_.pixel_format) == 0 || config_.frame_rate.num <= 0 ||
        config_.frame_rate.den <= 0 || config_.buffer_count < 2 || config_.buffer_count > 64 ||
        config_.poll_timeout.count() <= 0 || config_.poll_timeout.count() > INT_MAX) {
        return failed({CaptureErrorCategory::InvalidConfiguration, 0, "Invalid V4L2 capture configuration", false});
    }
    {
        std::lock_guard<std::mutex> wake_lock(wake_mutex_);
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    if (wake_fd_ < 0) return failed(NativeError("eventfd", errno));
    pool_ = std::make_shared<BufferPool>();
    device_fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    pool_->fd = device_fd_;
    if (device_fd_ < 0) return failed(NativeError("open capture device", errno));

    // 查询设备能力，确保是单平面视频采集设备
    v4l2_capability caps{};
    if (Ioctl(device_fd_, VIDIOC_QUERYCAP, &caps) < 0) {
        return failed(NativeError("QUERYCAP", errno));
    }
    const auto capabilities = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(capabilities & V4L2_CAP_STREAMING)) {
        return failed({CaptureErrorCategory::Unsupported, 0,
            "Requires single-plane Video Capture with Streaming (not output/metadata/mplane)", false});
    }

    // 设置视频采集格式
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<unsigned int>(config_.width);
    format.fmt.pix.height = static_cast<unsigned int>(config_.height);
    format.fmt.pix.pixelformat = Fourcc(config_.pixel_format);
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (Ioctl(device_fd_, VIDIOC_S_FMT, &format) < 0) return failed(NativeError("S_FMT", errno));
    if (Ioctl(device_fd_, VIDIOC_G_FMT, &format) < 0) return failed(NativeError("G_FMT", errno));
    const auto& pix = format.fmt.pix;
    if (pix.field != V4L2_FIELD_NONE || Pixel(pix.pixelformat) == PixelFormat::kUnknown ||
        pix.width == 0 || pix.height == 0 || pix.width > 16384 || pix.height > 16384 || pix.bytesperline == 0 || pix.sizeimage == 0) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Unsupported negotiated capture layout/field", false});
    }
    if (!config_.allow_negotiation && (pix.width != static_cast<unsigned int>(config_.width) ||
        pix.height != static_cast<unsigned int>(config_.height) || pix.pixelformat != Fourcc(config_.pixel_format))) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device adjusted requested capture format", false});
    }
    const auto pixel = Pixel(pix.pixelformat);
    const uint64_t minimum_stride = static_cast<uint64_t>(pix.width) * (pixel == PixelFormat::kYUYV422 ? 2 : 1);
    const uint64_t minimum_size = static_cast<uint64_t>(pix.bytesperline) * pix.height * (pixel == PixelFormat::kYUYV422 ? 2 : 3) / 2;
    if (pix.width % 2 != 0 || (pixel != PixelFormat::kYUYV422 && pix.height % 2 != 0) || (pixel == PixelFormat::kI420 && pix.bytesperline % 2 != 0) ||
        pix.bytesperline < minimum_stride || pix.sizeimage < minimum_size) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Invalid negotiated stride or image size", false});
    }

    // 设置视频采集帧率
    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (Ioctl(device_fd_, VIDIOC_G_PARM, &parameters) < 0) return failed(NativeError("G_PARM", errno));
    if (!(parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device has no configurable frame interval", false});
    }
    parameters.parm.capture.timeperframe = {static_cast<unsigned int>(config_.frame_rate.den), static_cast<unsigned int>(config_.frame_rate.num)};
    if (Ioctl(device_fd_, VIDIOC_S_PARM, &parameters) < 0) return failed(NativeError("S_PARM", errno));
    if (Ioctl(device_fd_, VIDIOC_G_PARM, &parameters) < 0) return failed(NativeError("G_PARM", errno));
    const auto interval = parameters.parm.capture.timeperframe;
    if (interval.numerator == 0 || interval.denominator == 0 || interval.numerator > INT_MAX || interval.denominator > INT_MAX ||
        (!config_.allow_negotiation && static_cast<uint64_t>(interval.numerator) * config_.frame_rate.num != static_cast<uint64_t>(interval.denominator) * config_.frame_rate.den)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device adjusted requested capture frame rate", false});
    }

    // 保存视频采集信息
    info_ = {static_cast<int>(pix.width), static_cast<int>(pix.height), Pixel(pix.pixelformat),
        {static_cast<int>(interval.denominator), static_cast<int>(interval.numerator)},
        pix.bytesperline, pix.sizeimage, 0, pix.colorspace, pix.ycbcr_enc, pix.quantization, pix.xfer_func};

    // 请求视频采集缓冲区
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = config_.buffer_count;
    if (Ioctl(device_fd_, VIDIOC_REQBUFS, &request) < 0) return failed(NativeError("REQBUFS", errno));
    if (request.count < 2 || request.count > 64) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Invalid returned capture buffer count", false});
    }
    info_.buffer_count = request.count;
    try {
        pool_->mappings.reserve(request.count);
        for (unsigned int index = 0; index < request.count; ++index) {
            if (stop_requested_.load()) return failed({CaptureErrorCategory::Cancelled, 0, "Capture open cancelled", false});
            v4l2_buffer buffer{};
            buffer.type = request.type;
            buffer.memory = request.memory;
            buffer.index = index;
            if (Ioctl(device_fd_, VIDIOC_QUERYBUF, &buffer) < 0) return failed(NativeError("QUERYBUF", errno));
            if (buffer.length < info_.size_image) {
                return failed({CaptureErrorCategory::InvalidFrame, 0, "Mapped buffer is smaller than sizeimage", false});
            }
            void* address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd_, buffer.m.offset);
            if (address == MAP_FAILED) return failed(NativeError("mmap", errno));
            pool_->mappings.push_back({address, buffer.length});
            if (Ioctl(device_fd_, VIDIOC_QBUF, &buffer) < 0) return failed(NativeError("initial QBUF", errno));
        }
    } catch (...) {
        cleanup();
        stop_requested_.store(true);
        throw;
    }
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (stop_requested_.load()) return failed({CaptureErrorCategory::Cancelled, 0, "Capture open cancelled", false});
    if (Ioctl(device_fd_, VIDIOC_STREAMON, &type) < 0) return failed(NativeError("STREAMON", errno));
    streaming_ = true;
    pool_->active = true;
    return {};
}

CaptureReadResult V4L2VideoCapture::ReadFrame() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (stop_requested_.load() || !streaming_) {
        return {CaptureReadStatus::Stopped, nullptr, std::nullopt};
    }

    // poll 事件循环
    pollfd descriptors[2]{{device_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
    int result;
    do {
        result = ::poll(descriptors, 2, static_cast<int>(config_.poll_timeout.count()));
    } while (result < 0 && errno == EINTR && !stop_requested_.load());
    if (stop_requested_.load() || descriptors[1].revents & POLLIN) {
        // 唤醒文件描述符被触发，说明上层请求停止采集视频
        return {CaptureReadStatus::Stopped, nullptr, std::nullopt};
    }


    if (result < 0) {
        return ReadFailure(NativeError("poll", errno));
    }
    if (result == 0) {
        return {CaptureReadStatus::NoData, nullptr, std::nullopt};
    }
    // 设备异常事件
    if (descriptors[0].revents & (POLLHUP | POLLNVAL | POLLERR)) {
        return ReadFailure({CaptureErrorCategory::DeviceDisconnected, 0, "Capture poll reported device failure", true});
    }


    // 出队缓冲区 DQBUF 操作
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;   // 映射内存
    if (Ioctl(device_fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return {CaptureReadStatus::NoData, nullptr, std::nullopt};
        }
        return ReadFailure(NativeError("DQBUF", errno));
    }
    if (buffer.index >= pool_->mappings.size()) {
        return ReadFailure({CaptureErrorCategory::InvalidFrame, 0, "Driver returned invalid buffer index", false});
    }

    // 处理捕获到的视频帧,复制视频帧数据到 MediaFrame
    const auto captured = buffer;
    auto guard = std::make_shared<DeferredBufferReturn>();
    guard->return_buffer = [pool = pool_, captured] { pool->Return(captured); };
    auto buffer_ptr = std::shared_ptr<const uint8_t>(
        static_cast<const uint8_t*>(pool_->mappings[buffer.index].address),
        [guard](const uint8_t*) {});  // guard 被 custom deleter 持有，QBUF 在 shared_ptr 析构时自动触发

    std::shared_ptr<MediaFrame> frame;
    std::optional<CaptureError> copy_error;
    try {
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            throw std::invalid_argument("Driver marked capture frame damaged");
        }
        if (buffer.field != V4L2_FIELD_NONE) {
            throw std::invalid_argument("Captured frame is not progressive");
        }
        if (buffer.bytesused > pool_->mappings[buffer.index].length) {
            throw std::invalid_argument("bytesused exceeds mapped buffer");
        }
        frame = MediaFrame::CreateCompact(std::move(buffer_ptr), buffer.bytesused,
            info_.pixel_format, info_.width, info_.height, static_cast<int>(info_.bytes_per_line));
        if (!frame) {
            copy_error = CaptureError{CaptureErrorCategory::InvalidFrame, 0, "CreateCompact failed", false};
        }
    } catch (const std::exception& error) {
        copy_error = CaptureError{CaptureErrorCategory::InvalidFrame, 0, error.what(), false};
    } catch (...) {
        copy_error = CaptureError{CaptureErrorCategory::System, 0, "Capture copy failed", false};
    }
    
    // QBUF 由 buffer_ptr 的 custom deleter 自动管理：
    //   零拷贝路径：SimpleBufferView 持有 shared_ptr -> 帧释放时 QBUF
    //   拷贝/失败路径：shared_ptr 在此作用域析构 -> 立即 QBUF
    //   异常路径：  栈展开 -> 立即 QBUF
    
    if (copy_error) {
        return ReadFailure(std::move(*copy_error));
    }

    // 处理时间戳
    const auto timestamp_type = captured.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
    const auto clock = timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ? CaptureTimestampClock::Monotonic : CaptureTimestampClock::Unknown;
    if (captured.timestamp.tv_sec >= 0 && captured.timestamp.tv_usec >= 0 && captured.timestamp.tv_usec < 1000000 
        && captured.timestamp.tv_sec <= (INT64_MAX - 999999) / 1000000) {
        frame->time.pts_us = static_cast<int64_t>(captured.timestamp.tv_sec) * 1000000 + captured.timestamp.tv_usec;
        frame->time.dts_us = frame->time.pts_us;
    }
    frame->time.duration_us = static_cast<int64_t>(info_.frame_rate.den) * 1000000 / info_.frame_rate.num;
    return {CaptureReadStatus::Frame, std::move(frame), std::nullopt, captured.sequence, clock};
}

void V4L2VideoCapture::RequestStop() noexcept {
    stop_requested_.store(true);
    std::lock_guard<std::mutex> lock(wake_mutex_);
    if (wake_fd_ >= 0) {
        const uint64_t one = 1;
        ssize_t result;
        do { result = ::write(wake_fd_, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    }
}

void V4L2VideoCapture::Close() noexcept {
    RequestStop();
    std::lock_guard<std::mutex> lock(io_mutex_);
    cleanup();
}

CaptureVideoInfo V4L2VideoCapture::GetVideoInfo() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return info_;
}

void V4L2VideoCapture::cleanup() noexcept {
    if (pool_) pool_->Stop();
    streaming_ = false;
    pool_.reset();
    device_fd_ = -1;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        if (wake_fd_ >= 0) ::close(wake_fd_);
        wake_fd_ = -1;
    }
    info_ = {};
}