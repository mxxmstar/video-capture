#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "media/capture/i_video_capture.h"

struct VideoCaptureSessionConfig {
    std::chrono::milliseconds idle_timeout{3000};  ///< 空闲超时时间，默认 3 秒
    std::chrono::milliseconds no_data_backoff{1};  ///< 无数据回退时间，默认 1 秒
};

/// @brief 视频捕获会话
class VideoCaptureSession {
public:
    enum class State { Idle, Running, Stopped, Failed };
    using FrameCallback = std::function<void(const CaptureReadResult&)>;

    /// @brief 构造函数
    /// @param capture 视频捕获设备指针
    /// @param config 会话配置
    explicit VideoCaptureSession(std::unique_ptr<IVideoCapture> capture, VideoCaptureSessionConfig config = {});
    ~VideoCaptureSession();
    VideoCaptureSession(const VideoCaptureSession&) = delete;
    VideoCaptureSession& operator=(const VideoCaptureSession&) = delete;

    /// @brief 设置帧回调函数
    /// @param callback 帧回调函数
    /// @note 回调函数将在捕获线程中调用，必须是无状态的
    void SetFrameCallback(FrameCallback callback);

    /// @brief 开始捕获会话
    /// @return 开启结果
    /// @note 开启结果为成功时，会话状态将变更为 Running
    CaptureOpenResult Start();

    /// @brief 请求停止捕获会话
    /// @note 会话状态将变更为 Stopped
    /// @note 会话状态为 Failed 时，会话将自动停止
    void RequestStop() noexcept;

    /// @brief 停止捕获会话
    /// @note 会话状态将变更为 Stopped
    /// @note 会话状态为 Failed 时，会话将自动停止
    void Stop();
    State GetState() const noexcept { return state_.load(); }
    CaptureVideoInfo GetVideoInfo() const;
    std::optional<CaptureError> LastError() const;
    std::uint64_t FramesReceived() const noexcept { return frames_.load(); }

private:
    /// @brief 运行捕获线程
    /// @note 该线程将在会话状态为 Running 时运行，负责从视频捕获设备读取帧并调用回调函数
    /// @note 该线程将在会话状态为 Stopped 时停止
    void run() noexcept;

    /// @brief 处理捕获错误
    /// @param error 错误信息
    /// @note 该函数将在捕获线程中调用，负责设置会话状态为 Failed
    void fail(CaptureError error);
    std::unique_ptr<IVideoCapture> capture_;    ///< 视频捕获设备指针
    VideoCaptureSessionConfig config_;  ///< 会话配置
    FrameCallback callback_;    ///< 帧回调函数
    std::thread worker_;        ///< 捕获线程
    std::atomic<bool> stopping_{true};  ///< 停止标志位
    std::atomic<State> state_{State::Idle};  ///< 会话状态
    std::atomic<std::uint64_t> frames_{0};  ///< 已接收帧数
    mutable std::mutex error_mutex_;     ///< 错误信息互斥锁
    std::optional<CaptureError> error_;  ///< 最后捕获的错误信息
};
