#include "media/media_frame.h"
#include "media/capture/video_capture_session.h"
#include "media/capture/v4l2_video_capture.h"
#include "media/ffmpeg_format.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class IdleCapture final : public IVideoCapture {
public:
    CaptureOpenResult Open() override { stopped.store(false); return {}; }
    CaptureReadResult ReadFrame() override {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(2), [this] { return stopped.load(); });
        return {stopped.load() ? CaptureReadStatus::Stopped : CaptureReadStatus::NoData, nullptr, std::nullopt};
    }
    void RequestStop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true);
        cv.notify_all();
    }
    void Close() noexcept override { RequestStop(); }
    CaptureVideoInfo GetVideoInfo() const override { return {}; }
    std::atomic<bool> stopped{true};
    std::mutex mutex;
    std::condition_variable cv;
};

class EmptyCapture final : public IVideoCapture {
public:
    CaptureOpenResult Open() override { return {}; }
    CaptureReadResult ReadFrame() override { return {CaptureReadStatus::NoData, nullptr, std::nullopt}; }
    void RequestStop() noexcept override {}
    void Close() noexcept override {}
    CaptureVideoInfo GetVideoInfo() const override { return {}; }
};

void TestLayout(PixelFormat format) {
    CaptureVideoInfo info;
    info.width = 4;
    info.height = 2;
    info.pixel_format = format;
    const int source_stride = format == PixelFormat::kYUYV422 ? 12 : 8;
    std::vector<unsigned char> source(24, 0xee);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<unsigned char>(i);
    const auto frame = MediaFrame::CreateCompact(source.data(), source.size(),
                                      info.pixel_format, 4, info.height,
                                      source_stride);
    Require(frame->view.TotalSize() == (format == PixelFormat::kYUYV422 ? 16U : 12U), "Packed size");
    Require(frame->Stride(0) == (format == PixelFormat::kYUYV422 ? 8 : 4), "Packed stride");
    Require(frame->view.Data(0)[frame->Stride(0)] == source[source_stride], "Padding was copied as pixels");
    if (format != PixelFormat::kYUYV422) {
        Require(frame->view.Data(1)[0] == source[16], "UV offset");
        if (format == PixelFormat::kI420) {
            Require(frame->view.Data(2)[0] == source[20], "V offset");
        }
    }
    const auto original = frame->view.Data(0)[0];
    source[0] = 255;
    Require(frame->view.Data(0)[0] == original, "Frame borrows source storage");
    Require(!MediaFrame::CreateCompact(source.data(), 1, info.pixel_format, 4,
        info.height, source_stride), "Truncated frame accepted");
}
}

int main() {
    try {
        TestLayout(PixelFormat::kYUYV422);
        TestLayout(PixelFormat::kNV12);
        TestLayout(PixelFormat::kI420);
        Require(Media::FromAVPixelFormat(Media::ToAVPixelFormat(PixelFormat::kYUYV422)) ==
            PixelFormat::kYUYV422, "YUYV roundtrip");
        V4L2CaptureConfig invalid;
        invalid.width = 3;
        V4L2VideoCapture invalid_capture(invalid);
        auto result = invalid_capture.Open();
        Require(result.error && result.error->category == CaptureErrorCategory::InvalidConfiguration, "Invalid config");
        invalid_capture.Close();
        invalid_capture.Close();
        V4L2CaptureConfig missing;
        missing.device = "/dev/nonexistent-video-capture-contract";
        V4L2VideoCapture missing_capture(missing);
        result = missing_capture.Open();
        Require(result.error && result.error->category == CaptureErrorCategory::NotFound, "Missing device category");
        VideoCaptureSession session(std::make_unique<IdleCapture>());
        Require(session.Start().Succeed(), "Session start");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto start = std::chrono::steady_clock::now();
        session.Stop();
        Require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500), "Blocked session did not stop");
        Require(session.GetState() == VideoCaptureSession::State::Stopped, "Session stop state");
        Require(session.Start().Succeed(), "Session restart");
        session.Stop();
        VideoCaptureSessionConfig timeout;
        timeout.idle_timeout = std::chrono::milliseconds(20);
        VideoCaptureSession empty(std::make_unique<EmptyCapture>(), timeout);
        Require(empty.Start().Succeed(), "Empty session start");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (empty.GetState() == VideoCaptureSession::State::Running && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        empty.Stop();
        Require(empty.LastError() && empty.LastError()->category == CaptureErrorCategory::Timeout, "Idle timeout");
        std::cout << "Capture contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}