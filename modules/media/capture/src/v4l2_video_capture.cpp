#include "common/log/logger.h"
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
    do {
        result = ::ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
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
    case V4L2_PIX_FMT_NV12M:
    case V4L2_PIX_FMT_NV12: return PixelFormat::kNV12;
    case V4L2_PIX_FMT_YUV420M:
    case V4L2_PIX_FMT_YUV420: return PixelFormat::kI420;
    default: return PixelFormat::kUnknown;
    }
}

CaptureError NativeError(const char* operation, int code) {
    auto category = CaptureErrorCategory::System;
    bool retryable = false;
    switch (code) {
    case ENOENT:
        category = CaptureErrorCategory::NotFound;
        retryable = true;
        break;
    case EACCES:
    case EPERM:
        category = CaptureErrorCategory::PermissionDenied;
        break;
    case EBUSY:
        category = CaptureErrorCategory::Busy;
        retryable = true;
        break;
    case ENODEV:
    case ENXIO:
        category = CaptureErrorCategory::DeviceDisconnected;
        retryable = true;
        break;
    case EINVAL:
    case ENOTTY:
        category = CaptureErrorCategory::Unsupported;
        break;
    case EIO:
        retryable = true;
        break;
    }
    return {category, code, std::string(operation) + ": " + std::strerror(code), retryable};
}

CaptureReadResult ReadFailure(CaptureError error) {
    const auto status = error.retryable ? CaptureReadStatus::RetryableError : CaptureReadStatus::FatalError;
    return {status, nullptr, std::move(error)};
}

/// @brief 平面最小空间参数结构体
struct PlaneRequirements {
    uint32_t min_stride{0}; ///< 最小步长,包含对齐像素
    uint64_t min_size{0}; ///< 最小图像大小
};

/// @brief 获取指定像素格式和尺寸的最小步长和最小图像大小,平面索引由调用方指定
/// @param format 像素格式
/// @param width 宽度
/// @param height 高度
/// @param plane_idx 平面索引
/// @param minimum_stride 最小步长
/// @return 平面最小空间参数
PlaneRequirements GetPlaneRequirements(::PixelFormat format, uint32_t width, uint32_t height, uint32_t plane_idx, uint32_t minimum_stride) {
    PlaneRequirements req;
    
    if (format == PixelFormat::kYUYV422) {
        // yuyv 通常为单平面
        req.min_stride = std::max(minimum_stride, width * 2);
        req.min_size = static_cast<uint64_t>(req.min_stride) * height;
    } else if (format == PixelFormat::kI420 || format == PixelFormat::kNV12 || format == PixelFormat::kNV21) {
        if (plane_idx == 0) {
            // Y 平面
            req.min_stride = std::max(minimum_stride, width);
            req.min_size = static_cast<uint64_t>(req.min_stride) * height;
        } else {
            // UV 平面
            uint32_t factor = (format == PixelFormat::kI420) ? 2 : 1;
            req.min_stride = std::max(minimum_stride / factor, width / factor);
            req.min_size = static_cast<uint64_t>(req.min_stride) * height / 2;
        }
    } else {
        throw std::runtime_error("Unsupported pixel format");
    }
    return req;
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

    // 查询设备能力，确保是平面视频采集设备
    v4l2_capability caps{};
    if (Ioctl(device_fd_, VIDIOC_QUERYCAP, &caps) < 0) {
        return failed(NativeError("QUERYCAP", errno));
    }
    const auto capabilities = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
    if (!(capabilities & V4L2_CAP_STREAMING)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device does not support streaming", false});
    }


    // 设置视频采集格式    
    v4l2_format format{};
    if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        format.fmt.pix_mp.width = static_cast<unsigned int>(config_.width);
        format.fmt.pix_mp.height = static_cast<unsigned int>(config_.height);
        format.fmt.pix_mp.pixelformat = Fourcc(config_.pixel_format);
        const auto separate_format = config_.pixel_format == PixelFormat::kNV12 ? V4L2_PIX_FMT_NV12M :
            (config_.pixel_format == PixelFormat::kI420 ? V4L2_PIX_FMT_YUV420M : Fourcc(config_.pixel_format));
        for (unsigned index = 0; ; ++index) {
            v4l2_fmtdesc descriptor{};
            descriptor.index = index;
            descriptor.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (Ioctl(device_fd_, VIDIOC_ENUM_FMT, &descriptor) < 0) break;
            if (descriptor.pixelformat == separate_format) {
                format.fmt.pix_mp.pixelformat = separate_format;
                break;
            }
        }
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        is_mplane_ = true;
    } else if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<unsigned int>(config_.width);
        format.fmt.pix.height = static_cast<unsigned int>(config_.height);
        format.fmt.pix.pixelformat = Fourcc(config_.pixel_format);
        format.fmt.pix.field = V4L2_FIELD_NONE;
        is_mplane_ = false;
    } else {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device does not support video capture", false});
    }

    // S_FMT / G_FMT（单/多平面共用，V4L2 根据 format.type 自动区分）
    if (Ioctl(device_fd_, VIDIOC_S_FMT, &format) < 0) {
        return failed(NativeError("S_FMT", errno));
    }
    if (Ioctl(device_fd_, VIDIOC_G_FMT, &format) < 0) {
        return failed(NativeError("G_FMT", errno));
    }

    // 提取单/多平面共用的协商后字段
    const auto width = is_mplane_ ? format.fmt.pix_mp.width : format.fmt.pix.width;
    const auto height = is_mplane_ ? format.fmt.pix_mp.height : format.fmt.pix.height;
    const auto pixelformat = is_mplane_ ? format.fmt.pix_mp.pixelformat : format.fmt.pix.pixelformat;
    const auto field = is_mplane_ ? format.fmt.pix_mp.field : format.fmt.pix.field;
    
    // 共用校验：field / 像素格式 / 宽高有效性
    if (field != V4L2_FIELD_NONE || Pixel(pixelformat) == PixelFormat::kUnknown ||
        width == 0 || height == 0 || width > 16384 || height > 16384) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Unsupported negotiated capture layout/field", false});
    }
    // 共用校验：不允许协商时检查设备是否调整了格式
    if (!config_.allow_negotiation && (width != static_cast<unsigned int>(config_.width) ||
        height != static_cast<unsigned int>(config_.height) || Pixel(pixelformat) != config_.pixel_format)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Device adjusted requested capture format", false});
    }
    const auto pixel = Pixel(pixelformat);
    // 计算最小步长，单平面则为 width * 2，多平面则为 width * 1
    const uint64_t minimum_stride = static_cast<uint64_t>(width) * (pixel == PixelFormat::kYUYV422 ? 2 : 1);
    // 共用校验：宽高偶对齐
    if (width % 2 != 0 || (pixel != PixelFormat::kYUYV422 && height % 2 != 0)) {
        return failed({CaptureErrorCategory::Unsupported, 0, "Invalid negotiated stride or image size", false});
    }

    // 各平面特有校验：bytesperline / sizeimage
    if (is_mplane_) {
        const auto& pix = format.fmt.pix_mp;
        if (pix.num_planes == 0 || pix.num_planes > VIDEO_MAX_PLANES) {
            return failed({CaptureErrorCategory::Unsupported, 0, "Unsupported negotiated capture layout/field", false});
        }
        // 多平面校验
        for (uint32_t i = 0; i < pix.num_planes; ++i) {
            const auto& plane = pix.plane_fmt[i];
            if (plane.bytesperline == 0 || plane.sizeimage == 0) {
                return failed({CaptureErrorCategory::Unsupported, 0, "Unsupported negotiated capture layout/field", false});
            }
            auto req = GetPlaneRequirements(pixel, width, height, i, minimum_stride);
            // 校验 I420 平面的 Y-stride 是否偶数齐
            if (pixel == PixelFormat::kI420 && i == 0 && (plane.bytesperline % 2 != 0)) {
                return failed({CaptureErrorCategory::Unsupported, 0, "I420 Y-stride must be even", false});
            }
            // 最小步长和最小图像大小校验
            if (plane.bytesperline < req.min_stride || plane.sizeimage < req.min_size) {
                return failed({CaptureErrorCategory::Unsupported, 0, "Invalid negotiated stride or image size in plane " + std::to_string(i), false});
            }
        }
        
    } else {
        const auto& pix = format.fmt.pix;
        if (pix.bytesperline == 0 || pix.sizeimage == 0) {
            return failed({CaptureErrorCategory::Unsupported, 0, "Unsupported negotiated capture layout/field", false});
        }
        const uint64_t minimum_size = static_cast<uint64_t>(pix.bytesperline) * height *
            (pixel == PixelFormat::kYUYV422 ? 2 : 3) / 2;
        if ((pixel == PixelFormat::kI420 && pix.bytesperline % 2 != 0) || pix.bytesperline < minimum_stride || pix.sizeimage < minimum_size) {
            return failed({CaptureErrorCategory::Unsupported, 0, "Invalid negotiated stride or image size", false});
        }
    }    

    // 设置视频采集帧率
    v4l2_streamparm parameters{};
    parameters.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
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

    // struct CaptureVideoInfo {
    // int width{0};                          // 帧宽度（像素）
    // int height{0};                         // 帧高度（像素）
    // PixelFormat pixel_format{PixelFormat::kUnknown};  // 像素格式（如 YUYV、MJPEG 等）
    // Rational frame_rate{0, 1};             // 帧率（分子/分母 表示，如 30/1 = 30fps）    
    // unsigned int size_image{0};            // 单帧图像的总字节数
    // unsigned int buffer_count{0};          // 缓冲区数量（用于多缓冲/队列采集）
    // unsigned int colorspace{0};            // V4L2 colorspace（如 sRGB、BT.709、BT.2020）
    // unsigned int ycbcr_encoding{0};        // YCbCr 编码标准（如 BT.601、BT.709、BT.2020）
    // unsigned int quantization{0};          // 量化范围（Full range / Limited range）
    // unsigned int transfer_function{0};     // 传输函数/光电转换函数（如 gamma、PQ、HLG）

    // std::size_t stride = 0;  ///< 行步长（包含填充字节）
    // std::size_t row_bytes = 0;  ///< 行字节数（实际有效数据大小）
    // std::size_t rows = 0;  ///< 行数

    // 保存视频帧几何信息
    std::size_t total_size_image = 0;
    if (is_mplane_) {
        for (uint32_t i = 0; i < format.fmt.pix_mp.num_planes; ++i) {
            auto bytesperline = format.fmt.pix_mp.plane_fmt[i].bytesperline;
            auto size_image = format.fmt.pix_mp.plane_fmt[i].sizeimage;
            std::size_t row_bytes = width;
            std::size_t rows = height;
            if (pixel == PixelFormat::kYUYV422) row_bytes = static_cast<std::size_t>(width) * 2;
            else if (pixel == PixelFormat::kI420 && i > 0) { row_bytes = width / 2; rows = height / 2; }
            else if (i > 0) rows = height / 2;
            plane_geometry_.push_back({static_cast<std::ptrdiff_t>(bytesperline), row_bytes, rows});
            total_size_image += size_image;
        }
    } else {
        const std::size_t row_bytes = pixel == PixelFormat::kYUYV422 ? static_cast<std::size_t>(width) * 2 : static_cast<std::size_t>(width);
        plane_geometry_.push_back({static_cast<std::ptrdiff_t>(format.fmt.pix.bytesperline), row_bytes, static_cast<std::size_t>(height)});
        total_size_image = format.fmt.pix.sizeimage;
    }

    // 保存视频采集信息
    info_ = {};
    info_.width = static_cast<int>(width);
    info_.height = static_cast<int>(height);
    info_.pixel_format = Pixel(pixelformat);
    info_.frame_rate = {static_cast<int>(interval.denominator), static_cast<int>(interval.numerator)};    
    info_.size_image = total_size_image;
    info_.colorspace = is_mplane_ ? format.fmt.pix_mp.colorspace : format.fmt.pix.colorspace;
    info_.ycbcr_encoding = is_mplane_ ? format.fmt.pix_mp.ycbcr_enc : format.fmt.pix.ycbcr_enc;
    info_.quantization = is_mplane_ ? format.fmt.pix_mp.quantization : format.fmt.pix.quantization;
    info_.transfer_function = is_mplane_ ? format.fmt.pix_mp.xfer_func : format.fmt.pix.xfer_func;    

    try {
        queue_ = std::make_shared<CaptureQueue>(device_fd_,
            is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);
        if (const int error = queue_->Initialize(config_.buffer_count, static_cast<unsigned>(info_.size_image), plane_geometry_)) {
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

    auto type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
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

        const auto lease_view = lease->View();
        const auto stride = plane_geometry_.empty() ? 0 : static_cast<int>(plane_geometry_[0].stride);
        const auto layout = PlaneLayout::Compute(info_.pixel_format, info_.width, info_.height, stride);
        if (layout.IsEmpty()) throw std::invalid_argument("Invalid captured frame layout");
        std::vector<MediaPlaneView> logical_views;
        logical_views.reserve(layout.planes.size());
        const bool source_is_contiguous = lease_view.PlaneCount() == 1;
        ConstBufferRegion source_region{};
        if (source_is_contiguous) {
            const auto base = lease_view.Data(0);
            source_region = {base.data(), base.size()};
            for (std::size_t i = 0; i < layout.planes.size(); ++i) {
                const auto& pl = layout.planes[i];
                const auto source_stride = static_cast<std::size_t>(pl.geometry.stride);
                const auto span = (pl.geometry.rows - 1) * source_stride + pl.geometry.row_bytes;
                if (pl.source_offset > base.size() || span > base.size() - pl.source_offset)
                    throw std::invalid_argument("Truncated captured logical plane");
                logical_views.emplace_back(base.data() + pl.source_offset, span,
                    pl.geometry);
            }
        } else {
            if (lease_view.PlaneCount() != layout.planes.size())
                throw std::invalid_argument("Unexpected captured memory plane count");
            for (std::size_t i = 0; i < layout.planes.size(); ++i) {
                const auto* source = lease_view.Plane(i);
                const auto& pl = layout.planes[i];
                if (!source || static_cast<std::size_t>(i) >= plane_geometry_.size())
                    throw std::invalid_argument("Missing captured memory plane");
                logical_views.emplace_back(source->data, source->size,
                    PlaneGeometry{plane_geometry_[i].stride, pl.geometry.row_bytes, pl.geometry.rows});
            }
        }
        for (const auto& pl : logical_views) {
            if (!pl.IsValid()) throw std::invalid_argument("Invalid captured plane span");
        }
        frame = std::make_shared<MediaFrame>();
        VideoFrameMeta meta{};
        meta.width = info_.width;
        meta.height = info_.height;
        meta.pixel_format = info_.pixel_format;
        frame->meta = meta;
        if (config_.delivery_mode == V4L2DeliveryMode::CopyCompact || config_.delivery_mode == V4L2DeliveryMode::Copy) {
            MediaBufferView source_view(std::move(logical_views), lease);
            auto owned = config_.delivery_mode == V4L2DeliveryMode::CopyCompact
                ? MediaBuffer::CopyContiguous(source_view)
                : MediaBuffer::CopySeparatePlanes(source_view);
            if (!owned) throw std::invalid_argument("Failed to copy captured frame planes");
            frame->view = MediaBufferView::FromOwner(std::move(owned));
        } else {
            frame->view = source_is_contiguous
                ? MediaBufferView::FromContiguous(std::move(logical_views), source_region, lease)
                : MediaBufferView::FromPlanes(std::move(logical_views), lease);
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
        do {
            result = ::write(wake_fd_, &one, sizeof(one));
        } while (result < 0 && errno == EINTR);
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

std::vector<PlaneGeometry> V4L2VideoCapture::Geometry() const {    
    return plane_geometry_;
}

void V4L2VideoCapture::cleanup() noexcept {
    if (streaming_) {
        auto type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
    plane_geometry_.clear();
}
