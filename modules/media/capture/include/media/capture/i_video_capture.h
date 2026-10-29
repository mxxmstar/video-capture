#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "media/media_frame.h"

enum class CaptureErrorCategory {
    InvalidConfiguration,
    NotFound,
    PermissionDenied,
    Busy,
    Unsupported,
    DeviceDisconnected,
    Timeout,
    Cancelled,
    InvalidFrame,
    System,
};

struct CaptureError {
    CaptureErrorCategory category;
    int native_code{0};
    std::string message;
    bool retryable{false};
};

struct CaptureOpenResult {
    std::optional<CaptureError> error;
    bool Succeed() const noexcept { return !error; }
};

enum class CaptureReadStatus {
    Frame, NoData, Stopped, RetryableError, FatalError,
};
enum class CaptureTimestampClock {
    Unknown,
    Monotonic,  ///< 单调时钟，不受系统时间调整影响
    Realtime,   ///< 实时时钟，受系统时间调整影响
};

/// @brief 描述视频采集设备输出格式与缓冲区配置的纯数据结构
struct CaptureVideoInfo {
    int width{0};                          // 帧宽度（像素）
    int height{0};                         // 帧高度（像素）
    PixelFormat pixel_format{PixelFormat::kUnknown};  // 像素格式（如 YUYV、MJPEG 等）
    Rational frame_rate{0, 1};             // 帧率（分子/分母 表示，如 30/1 = 30fps）        
    std::size_t size_image{0};            // 单帧图像的总字节数
    unsigned int buffer_count{0};          // 缓冲区数量（用于多缓冲/队列采集）
    unsigned int colorspace{0};            // V4L2 colorspace（如 sRGB、BT.709、BT.2020）
    unsigned int ycbcr_encoding{0};        // YCbCr 编码标准（如 BT.601、BT.709、BT.2020）
    unsigned int quantization{0};          // 量化范围（Full range / Limited range）
    unsigned int transfer_function{0};     // 传输函数/光电转换函数（如 gamma、PQ、HLG）
};

struct CaptureReadResult {
    CaptureReadStatus status{CaptureReadStatus::FatalError};     ///< 读取状态
    std::shared_ptr<MediaFrame> frame;                          ///< 帧数据指针
    std::optional<CaptureError> error;                          ///< 错误信息
    unsigned int sequence{0};                                 ///< 帧序号
    CaptureTimestampClock clock{CaptureTimestampClock::Unknown};///< 时间戳时钟
};

class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;
    virtual CaptureOpenResult Open() = 0;
    virtual CaptureReadResult ReadFrame() = 0;    
    virtual void RequestStop() noexcept = 0;
    virtual void Close() noexcept = 0;
    virtual CaptureVideoInfo GetVideoInfo() const = 0;
};
