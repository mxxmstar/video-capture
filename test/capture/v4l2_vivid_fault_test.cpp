#include "media/capture/v4l2_video_capture.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

/// @file v4l2_vivid_fault_test.cpp
/// @brief 测试 V4L2 视频捕获设备的故障注入
/// @param device V4L2 设备路径
/// @param id 指令 ID


namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Inject(const std::string& device, unsigned id) {
    const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    Require(fd >= 0, "Cannot open vivid control descriptor");
    v4l2_control control{};
    control.id = id;
    const int result = ::ioctl(fd, VIDIOC_S_CTRL, &control);
    ::close(fd);
    Require(result == 0, "Cannot inject vivid error (this test requires vivid)");
}
CaptureReadResult Read(V4L2VideoCapture& capture) {
    for (int i = 0; i < 50; ++i) {
        auto result = capture.ReadFrame();
        if (result.status != CaptureReadStatus::NoData) return result;
    }
    throw std::runtime_error("Frame deadline exceeded");
}
void Open(V4L2VideoCapture& capture) {
    auto result = capture.Open();
    if (result.error) throw std::runtime_error(result.error->message);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: v4l2_vivid_fault_test VIVID_DEVICE");
        V4L2CaptureConfig config;
        config.device = argv[1];
        // 0x00f0f042	VIDIOC_QBUF	给下一帧标记 V4L2_BUF_FLAG_ERROR（损坏帧）
        // 0x00f0f043	VIDIOC_REQBUFS	REQBUFS 返回失败
        // 0x00f0f044	VIDIOC_QBUF	QBUF 返回失败
        // 0x00f0f045	VIDIOC_STREAMON	STREAMON 返回失败
        for (unsigned control : {0x00f0f043u, 0x00f0f044u, 0x00f0f045u}) {
            Inject(config.device, control); // REQBUFS, initial QBUF, STREAMON
            V4L2VideoCapture capture(config);
            Require(!capture.Open().Succeed(), "Injected open failure was ignored");
            capture.Close();
            Open(capture);
            Require(Read(capture).status == CaptureReadStatus::Frame, "Reopen after failure failed");
        }
        V4L2VideoCapture capture(config);
        Open(capture);
        Inject(config.device, 0x00f0f042u); // V4L2_BUF_FLAG_ERROR
        auto damaged = Read(capture);
        // A frame completed before S_CTRL may already be waiting in the driver.
        for (unsigned i = 0; i < capture.GetVideoInfo().buffer_count && damaged.status == CaptureReadStatus::Frame; ++i) {
            damaged = Read(capture);
        }            
        Require(damaged.error && damaged.error->category == CaptureErrorCategory::InvalidFrame, "Damaged frame did not surface InvalidFrame");
        auto frame = Read(capture);
        Require(frame.status == CaptureReadStatus::Frame, "Damaged slot was not recycled");
        Inject(config.device, 0x00f0f044u);
        frame.frame.reset();
        auto failed = capture.ReadFrame();
        Require(failed.error && failed.error->native_code != 0, "Deferred QBUF failure was swallowed");
        Require(capture.ReadFrame().error.has_value(), "Failed queue resumed without reopen");
        capture.Close();
        Open(capture);
        Require(Read(capture).status == CaptureReadStatus::Frame, "Reopen after recycle failure failed");
        std::cout << "vivid allocation, initial queue, stream start, damaged frame and recycle faults passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
