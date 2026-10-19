#include "media/capture/v4l2_video_capture.h"
#include "media/capture/capture_queue.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
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
        config_.poll_timeout.count() <= 0 || config_.poll_timeout.count() > INT_MAX ||
        (config_.delivery_mode != V4L2DeliveryMode::Copy && config_.delivery_mode != V4L2DeliveryMode::MmapLease)) {
        return failed({CaptureErrorCategory::InvalidConfiguration, 0, "Invalid V4L2 capture configuration", false});
    }
    {
        std::lock_guard<std::mutex> wake_lock(wake_mutex_);
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    if (wake_fd_ < 0) {
        return failed(NativeError("eventfd", errno));
    }
    device_fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device_fd_ < 0) {
        return failed(NativeError("open capture device", errno));
    }

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
    if (Ioctl(device_fd_, VIDIOC_S_FMT, &format) < 0) {
        return failed(NativeError("S_FMT", errno));
    }
    if (Ioctl(device_fd_, VIDIOC_G_FMT, &format) < 0) {
        return failed(NativeError("G_FMT", errno));
    }
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
    if (Ioctl(device_fd_, VIDIOC_S_PARM, &parameters) < 0) {
        return failed(NativeError("S_PARM", errno));
    }
    if (Ioctl(device_fd_, VIDIOC_G_PARM, &parameters) < 0) {
        return failed(NativeError("G_PARM", errno));
    }
    const auto interval = parameters.parm.capture.timeperframe;
    if (interval.numerator == 0 || interval.denominator == 0 || interval.numerator > INT_MAX || interval.denominator > INT_MAX ||
        (!config_.allow_negotiation && static_cast<uint64_t>(interval.numerator) * config_.frame_rate.num != static_cast<uint64_t>(interval.denominator) * config_.frame_rate.den)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device adjusted requested capture frame rate", false});
    }

    // 保存视频采集信息
    info_ = {static_cast<int>(pix.width), static_cast<int>(pix.height), Pixel(pix.pixelformat),
        {static_cast<int>(interval.denominator), static_cast<int>(interval.numerator)},
        pix.bytesperline, pix.sizeimage, 0, pix.colorspace, pix.ycbcr_enc, pix.quantization, pix.xfer_func};

    try {
        queue_ = std::make_shared<CaptureQueue>(device_fd_);
        if (const int error = queue_->Initialize(config_.buffer_count, info_.size_image)) {
            return failed(NativeError("initialize capture queue", error));
        }
        info_.buffer_count = queue_->Count();
    } catch (const std::system_error& error) {
        return failed(NativeError("initialize capture queue", error.code().value()));
    } catch (...) {
        cleanup();
        stop_requested_.store(true);
        throw;
    }

    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (stop_requested_.load()) {
        LOG_WARN("Capture open cancelled");
        return failed({CaptureErrorCategory::Cancelled, 0, "Capture open cancelled", false});
    }
    if (Ioctl(device_fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start streaming: {}", errno);
        return failed(NativeError("STREAMON", errno));
    }
    streaming_ = true;
    return {};
}

CaptureReadResult V4L2VideoCapture::ReadFrame() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (stop_requested_.load() || !streaming_) {
        return {CaptureReadStatus::Stopped, nullptr, std::nullopt};
    }

    // 归还上一轮用完的buffer
    if (const int error = queue_->DrainReturns()) {
        return ReadFailure(NativeError("recycle QBUF", error));
    }

    // 三路 poll: device_fd_ v4l2视频数据就绪，wake_fd_ 用于接收上层的请求停止采集视频事件，queue_->WakeFd() 用于接收归还事件
    pollfd descriptors[3]{{queue_->HasQueued() ? device_fd_ : -1, POLLIN, 0},   // HasQueued为false表示所有的slot都被消费者占用, 不再等待视频帧事件
                          {wake_fd_, POLLIN, 0}, {queue_->WakeFd(), POLLIN, 0}};
    int result;
    do {
        result = ::poll(descriptors, 3, static_cast<int>(config_.poll_timeout.count()));
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
    // 归还事件触发
    if (descriptors[2].revents & POLLIN) {
        if (const int error = queue_->DrainReturns()) {
            return ReadFailure(NativeError("recycle QBUF", error));
        }
        if (!(descriptors[0].revents & POLLIN)) {
            return {CaptureReadStatus::NoData, nullptr, std::nullopt};  // 没有视频数据就绪, 直接返回空
        }
    }
    // 设备异常事件
    if (descriptors[0].revents & (POLLHUP | POLLNVAL | POLLERR)) {
        return ReadFailure({CaptureErrorCategory::DeviceDisconnected, 0, "Capture poll reported device failure", true});
    }


    // 出队缓冲区 DQBUF 操作
    v4l2_buffer buffer{};
    std::shared_ptr<FrameLease> lease;
    if (const int error = queue_->Dequeue(buffer, lease)) {
        if (error == EAGAIN) {
            return {CaptureReadStatus::NoData, nullptr, std::nullopt};  // 被抢先了, 直接返回空
        }
        if (error == EPROTO) {
            LOG_WARN("Driver returned invalid capture buffer index, state or size: {}", error);
            return ReadFailure({CaptureErrorCategory::InvalidFrame, error,
                "Driver returned invalid capture buffer index, state or size", false});
        }
        return ReadFailure(NativeError("DQBUF", error));
    }

    // 处理视频数据
    const auto& captured = buffer;   // 拷贝一份快照，避免修改原始数据，目前可以不需要拷贝，创建引用，后续需要修改buffer时需要改为拷贝

    std::shared_ptr<MediaFrame> frame;
    std::optional<CaptureError> copy_error;
    try {
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            LOG_WARN("Driver marked capture frame damaged");
            throw std::invalid_argument("Driver marked capture frame damaged");
        }
        if (buffer.field != V4L2_FIELD_NONE) {
            LOG_WARN("Captured frame is not progressive");
            throw std::invalid_argument("Captured frame is not progressive");
        }

        if (config_.delivery_mode == V4L2DeliveryMode::Copy) {
            frame = MediaFrame::CreateCompact(lease->Data(), lease->Size(), info_.pixel_format, info_.width, info_.height, static_cast<int>(info_.bytes_per_line));
        } else {
            const auto layout = SourcePlaneLayout::Compute(info_.pixel_format, info_.width, info_.height, static_cast<int>(info_.bytes_per_line));
            if (layout.is_empty()) {
                throw std::invalid_argument("Invalid leased frame layout");
            }
            VideoFrameMeta meta{};
            meta.width = info_.width;
            meta.height = info_.height;
            meta.pixel_format = info_.pixel_format;
            meta.plane_count = layout.plane_count;
            for (int i = 0; i < layout.plane_count; ++i) {
                const auto& plane = layout.planes[i];
                const auto span = static_cast<std::size_t>(plane.rows - 1) * plane.source_stride + plane.row_bytes;
                if (plane.source_offset > INT_MAX || span > INT_MAX || plane.source_offset > lease->Size() || span > lease->Size() - plane.source_offset) {
                    throw std::invalid_argument("Truncated leased frame plane");
                }                    
                meta.plane_info[i] = {static_cast<int>(plane.source_offset), plane.source_stride, static_cast<int>(span)};
            }
            frame = std::make_shared<MediaFrame>();
            frame->meta = meta;
            frame->buffer = lease;
        }
        if (!frame) {
            copy_error = CaptureError{CaptureErrorCategory::InvalidFrame, 0, "CreateCompact failed", false};
        }
    } catch (const std::exception& error) {
        copy_error = CaptureError{CaptureErrorCategory::InvalidFrame, 0, error.what(), false};
    } catch (...) {
        copy_error = CaptureError{CaptureErrorCategory::System, 0, "Capture copy failed", false};
    }
    
    lease.reset();
    if (const int error = queue_->DrainReturns()) {
        return ReadFailure(NativeError("recycle QBUF", error));
    }
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
    if (streaming_) {
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Ioctl(device_fd_, VIDIOC_STREAMOFF, &type) < 0) {
            LOG_WARN("Capture STREAMOFF failed: {}", std::strerror(errno));
        }
    }
    streaming_ = false;
    if (queue_) {
        if (const int error = queue_->Retire()) {
            LOG_WARN("Cannot release capture queue (driver may not support orphaned buffers): {}", std::strerror(error));
        }
        queue_.reset();
    }
    if (device_fd_ >= 0) {
        ::close(device_fd_);
    }
    device_fd_ = -1;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
        }
        wake_fd_ = -1;
    }
    info_ = {};
}
