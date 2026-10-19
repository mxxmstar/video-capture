#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "media/capture/i_video_capture.h"

/// @brief V4L2 视频捕获模式
enum class V4L2DeliveryMode {
    /// @brief 将有效行复制到独立紧凑缓冲区，拷贝结束后归还驱动 slot，与 buffer 完全解耦
    Copy,
    /// @brief 不拷贝任何数据，MediaFrame 直接指向 buffer 内存
    MmapLease,
};

class CaptureQueue;

struct V4L2CaptureConfig {
    std::string device{"/dev/video0"};
    int width{640};
    int height{360};
    PixelFormat pixel_format{PixelFormat::kYUYV422};
    Rational frame_rate{5, 1};
    unsigned int buffer_count{4};
    std::chrono::milliseconds poll_timeout{100};    
    bool allow_negotiation{false};  ///< 是否允许调整大小、格式或帧率
    V4L2DeliveryMode delivery_mode{V4L2DeliveryMode::MmapLease};
};

// Linux single-memory-plane streaming capture with explicit delivery ownership.
class V4L2VideoCapture final : public IVideoCapture {
public:
    explicit V4L2VideoCapture(V4L2CaptureConfig config);
    ~V4L2VideoCapture() override;
    V4L2VideoCapture(const V4L2VideoCapture&) = delete;
    V4L2VideoCapture& operator=(const V4L2VideoCapture&) = delete;

    CaptureOpenResult Open() override;
    CaptureReadResult ReadFrame() override;
    void RequestStop() noexcept override;
    void Close() noexcept override;
    CaptureVideoInfo GetVideoInfo() const override;

private:
    void cleanup() noexcept;
    V4L2CaptureConfig config_;
    mutable std::mutex io_mutex_;   ///< I/O 操作互斥锁
    std::mutex wake_mutex_;         ///< 唤醒互斥锁
    std::atomic<bool> stop_requested_{true};    ///< 请求停止
    int device_fd_{-1};                        ///< 视频设备文件描述符
    int wake_fd_{-1};                          ///< 唤醒文件描述符
    bool streaming_{false};                   ///< 是否正在流式传输
    std::shared_ptr<CaptureQueue> queue_;
    CaptureVideoInfo info_;
};
