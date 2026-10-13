#include "media/capture/video_capture_session.h"
#include "common/log/logger.h"


#include <stdexcept>
#include <utility>

namespace {
thread_local const VideoCaptureSession* active_session = nullptr;
}

VideoCaptureSession::VideoCaptureSession(std::unique_ptr<IVideoCapture> capture, VideoCaptureSessionConfig config) 
    : capture_(std::move(capture)), config_(config) {
    if (!capture_ || config_.idle_timeout.count() <= 0 || config_.no_data_backoff.count() < 0) {
        throw std::invalid_argument("Invalid video capture session configuration");
    }
}

VideoCaptureSession::~VideoCaptureSession() { Stop(); }

void VideoCaptureSession::SetFrameCallback(FrameCallback callback) {
    if (worker_.joinable()) throw std::logic_error("Stop session before changing callback");
    callback_ = std::move(callback);
}

CaptureOpenResult VideoCaptureSession::Start() {
    if (active_session == this) {
        throw std::logic_error("Start cannot run in capture callback");
    }

    Stop();
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_.reset();
    }
    frames_.store(0);
    auto result = capture_->Open();
    if (!result.Succeed()) {
        capture_->Close();
        fail(*result.error);
        return result;
    }
    stopping_.store(false);
    state_.store(State::Running);
    try {
        worker_ = std::thread(&VideoCaptureSession::run, this);
    } catch (...) {
        stopping_.store(true);
        capture_->Close();
        fail({CaptureErrorCategory::System, 0, "Cannot start capture worker", false});
        throw;
    }
    return {};
}

void VideoCaptureSession::RequestStop() noexcept {
    stopping_.store(true);
    capture_->RequestStop();
}

void VideoCaptureSession::Stop() {
    if (active_session == this) {
        throw std::logic_error("Use RequestStop in a capture callback");
    }
    RequestStop();
    if (worker_.joinable()) {
        worker_.join();
    }
    capture_->Close();
    if (state_.load() != State::Failed) {
        state_.store(State::Stopped);
    }
}

CaptureVideoInfo VideoCaptureSession::GetVideoInfo() const { return capture_->GetVideoInfo(); }

std::optional<CaptureError> VideoCaptureSession::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void VideoCaptureSession::fail(CaptureError error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = std::move(error);
    state_.store(State::Failed);
}

void VideoCaptureSession::run() noexcept {
    active_session = this;
    auto last_frame = std::chrono::steady_clock::now();
    try {
        while (!stopping_.load()) {
            auto result = capture_->ReadFrame();
            if (stopping_.load()) break;
            if (result.status == CaptureReadStatus::Frame) {
                if (!result.frame || !result.frame->buffer) {
                    fail({CaptureErrorCategory::InvalidFrame, 0, "Capture returned an empty frame", false});
                    break;
                }
                last_frame = std::chrono::steady_clock::now();
                ++frames_;
                if (callback_) callback_(result);
            } else if (result.status == CaptureReadStatus::NoData) {
                if (std::chrono::steady_clock::now() - last_frame >= config_.idle_timeout) {
                    fail({CaptureErrorCategory::Timeout, 0, "Capture idle timeout", true});
                    break;
                }
                std::this_thread::sleep_for(config_.no_data_backoff);
            } else if (result.status == CaptureReadStatus::Stopped) {
                break;
            } else {
                fail(result.error.value_or(CaptureError{CaptureErrorCategory::System,
                    0, "Capture failed without an error", false}));
                break;
            }
        }
    } catch (const std::exception& error) {
        fail({CaptureErrorCategory::System, 0, error.what(), false});
    } catch (...) {
        fail({CaptureErrorCategory::System, 0, "Capture callback raised an exception", false});
    }
    stopping_.store(true);
    capture_->Close();
    if (state_.load() != State::Failed) state_.store(State::Stopped);
    active_session = nullptr;
}
