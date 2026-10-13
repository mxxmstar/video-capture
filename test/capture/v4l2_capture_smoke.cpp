#include "media/capture/v4l2_video_capture.h"
#include "media/capture/video_capture_session.h"
#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/publisher/publisher.h"

#include "render/video/opengl_video_renderer.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void OpenOrThrow(V4L2VideoCapture& capture) {
    const auto result = capture.Open();
    if (result.error) throw std::runtime_error(result.error->message);
}
CaptureReadResult ReadOrThrow(V4L2VideoCapture& capture) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = capture.ReadFrame();
        if (result.status == CaptureReadStatus::Frame) return result;
        if (result.status != CaptureReadStatus::NoData)
            throw std::runtime_error(result.error ? result.error->message : "Capture stopped unexpectedly");
    }
    throw std::runtime_error("No capture frame within 5 seconds");
}

void testVideoCaptureRender(const char* device, const std::string& format) {
    V4L2CaptureConfig config;
    config.device = device;
    if (format == "nv12") config.pixel_format = PixelFormat::kNV12;
    else if (format == "i420") config.pixel_format = PixelFormat::kI420;
    else if (format != "yuyv") throw std::runtime_error("Unknown test format");

    V4L2VideoCapture capture(config);
    OpenOrThrow(capture);
    const auto info = capture.GetVideoInfo();
    std::cout << "Capture: " << info.width << 'x' << info.height
              << " fps=" << info.frame_rate.toString() << '\n';

    render::RenderConfig render_config;
    render_config.window_width = info.width;
    render_config.window_height = info.height;
    render_config.visible = true;
    render_config.vsync = false;
    render_config.title = "V4L2 Capture Render";

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(render_config)) {
        throw std::runtime_error(renderer.LastError());
    }

    MediaFrameConverter converter;
    if (config.pixel_format == PixelFormat::kYUYV422) {
        MediaFrameConverterConfig conversion;
        conversion.video = {info.width, info.height, PixelFormat::kI420, 2};
        if (!converter.Open(conversion)) {
            throw std::runtime_error("Cannot open converter for YUYV->I420");
        }
    }

    int frame_count = 0;
    while (!renderer.ShouldClose()) {
        auto result = capture.ReadFrame();
        if (result.status == CaptureReadStatus::NoData) {
            renderer.PollEvents();
            continue;
        }
        if (result.status != CaptureReadStatus::Frame) break;

        const MediaFrame* render_frame = result.frame.get();
        std::shared_ptr<MediaFrame> converted;
        if (config.pixel_format == PixelFormat::kYUYV422) {
            if (!converter.Convert(*result.frame, converted)) {
                std::cerr << "Conversion failed at frame " << frame_count << '\n';
                break;
            }
            render_frame = converted.get();
        }

        if (!renderer.Render(*render_frame)) {
            std::cerr << "Render failed at frame " << frame_count << ": "
                      << renderer.LastError() << '\n';
            break;
        }

        ++frame_count;
        std::cout << "Rendered frame " << frame_count << '\r' << std::flush;
        renderer.PollEvents();
    }

    std::cout << "\nRendered " << frame_count << " frames total\n";
    renderer.Shutdown();
    capture.Close();
}

}


int main(int argc, char** argv) {
    testVideoCaptureRender("/dev/video0", "yuyv");

    // if (argc != 4) {
    //     std::cerr << "Usage: v4l2_capture_smoke DEVICE yuyv|nv12|i420 OUTPUT.mp4\n";
    //     return 2;
    // }
    // try {
    //     V4L2CaptureConfig config;
    //     config.device = argv[1];
    //     const std::string format = argv[2];
    //     if (format == "nv12") config.pixel_format = PixelFormat::kNV12;
    //     else if (format == "i420") config.pixel_format = PixelFormat::kI420;
    //     else if (format != "yuyv") throw std::runtime_error("Unknown test format");
    //     V4L2VideoCapture capture(config);
    //     OpenOrThrow(capture);
    //     const auto info = capture.GetVideoInfo();
    //     std::cout << "Negotiated " << info.width << 'x' << info.height
    //         << " stride=" << info.bytes_per_line << " sizeimage=" << info.size_image
    //         << " buffers=" << info.buffer_count << " fps=" << info.frame_rate.toString() << '\n';

    //     // 缓冲区复用验证
    //     auto first = ReadOrThrow(capture);
    //     Require(IsValidTimestamp(first.frame->time.pts_us), "Missing capture timestamp");
    //     const auto saved = first.frame->buffer->Data()[0];
    //     for (int i = 0; i < 3; ++i) ReadOrThrow(capture);
    //     capture.Close();
    //     // 关闭后，缓冲区应保持不变
    //     Require(first.frame->buffer->Data()[0] == saved, "Copy changed after requeue or Close");

    //     // 重复开关与跨线程释放旧帧验证
    //     for (int i = 0; i < 5; ++i) {
    //         OpenOrThrow(capture);
    //         auto current = ReadOrThrow(capture);
    //         if (first.frame) Require(first.frame->buffer->Data()[0] == saved, "Old mapping changed after reopen");
    //         if (i == 0) {
    //             // Release an old frame on another thread while the new pool is active.
    //             auto old = std::move(first.frame);
    //             auto release = std::async(std::launch::async, [old = std::move(old)]() mutable { old.reset(); });
    //             release.get();
    //         }
    //         capture.Close();
    //         capture.Close();
    //     }

    //     // A slow source leaves poll blocked well beyond the required stop deadline.
    //     auto slow = config;
    //     slow.frame_rate = {1, 1};   // 1 fps
    //     slow.poll_timeout = std::chrono::seconds(3);     // 3s 超时
    //     V4L2VideoCapture idle(slow);
    //     OpenOrThrow(idle);
    //     ReadOrThrow(idle);
    //     // 启动异步读取线程
    //     auto pending = std::async(std::launch::async, [&] { return idle.ReadFrame(); });
    //     // 等 30ms 确认 ReadFrame 已经在 poll 中阻塞
    //     std::this_thread::sleep_for(std::chrono::milliseconds(30));
    //     Require(pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout, "Stop test did not reach a blocked read");
    //     const auto stop_start = std::chrono::steady_clock::now();
    //     idle.RequestStop();
    //     // 验证 eventfd 能在 500ms 内中断 poll，断言异步的 ReadFrame 线程已经完成
    //     Require(pending.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready, "eventfd did not interrupt poll within 500ms");
    //     Require(pending.get().status == CaptureReadStatus::Stopped, "Read did not report Stopped");
    //     idle.Close();
    //     std::cout << "Blocked read stop: " << std::chrono::duration_cast<std::chrono::microseconds>(
    //         std::chrono::steady_clock::now() - stop_start).count() << " us\n";        

    //     MediaFrameConverter converter;
    //     MediaFrameConverterConfig conversion;
    //     conversion.video = {config.width, config.height, PixelFormat::kI420, 2};
    //     Require(converter.Open(conversion), "Cannot open converter");
    //     FFmpegEncoder encoder;
    //     EncoderConfig encoding;
    //     VideoEncoderConfig video;
    //     video.width = config.width;
    //     video.height = config.height;
    //     video.fps_num = config.frame_rate.num;
    //     video.fps_den = config.frame_rate.den;
    //     encoding.specific = video;
    //     encoding.encoder_name = "libx264";
    //     encoding.global_header = true;
    //     Require(encoder.Open(encoding), "Cannot open H264 encoder");
    //     const auto track = encoder.GetOutputInfo();
    //     PublisherConfig publishing;
    //     publishing.targets.push_back({"capture-file", {}});
    //     auto& target = publishing.targets.front().session;
    //     target.reconnect_policy.enabled = false;
    //     target.pusher.output_url = argv[3];
    //     target.pusher.ffmpeg.output_format = "mp4";
    //     auto& output_track = target.pusher.video_track;
    //     output_track.codec_type = track.codec_type;
    //     output_track.time_base_num = track.time_base.num;
    //     output_track.time_base_den = track.time_base.den;
    //     output_track.extra_data = track.extra_data;
    //     output_track.video() = {track.video().width, track.video().height, track.video().fps};
    //     Publisher publisher;
    //     const auto published_open = publisher.Open(publishing);
    //     if (published_open.error) throw std::runtime_error(published_open.error->message);
    //     std::uint64_t packet_count = 0;
    //     int64_t previous_dts = kNoTimestamp;
    //     const auto write_packets = [&](const std::vector<PacketPtr>& packets) {
    //         for (const auto& packet : packets) {
    //             Require(packet && packet->buffer && packet->buffer->IsContiguous(), "Invalid encoded packet");
    //             Require(IsValidTimestamp(packet->dts) && (!IsValidTimestamp(previous_dts) ||
    //                 packet->dts > previous_dts), "Invalid encoded DTS");
    //             previous_dts = packet->dts;
    //             const auto result = publisher.Publish(*packet);
    //             Require(result.WasPublished(), "Publisher failed to write capture packet");
    //             ++packet_count;
    //         }
    //     };
    //     std::mutex mutex;
    //     std::condition_variable cv;
    //     std::atomic<int> frames{0};
    //     int64_t epoch = kNoTimestamp;
    //     int64_t previous = kNoTimestamp;
    //     VideoCaptureSession session(std::make_unique<V4L2VideoCapture>(config));
    //     session.SetFrameCallback([&](const CaptureReadResult& result) {
    //         auto frame = result.frame;
    //         Require(IsValidTimestamp(frame->time.pts_us), "Missing session timestamp");
    //         Require(!IsValidTimestamp(previous) || frame->time.pts_us > previous, "Non-monotonic capture timestamp");
    //         previous = frame->time.pts_us;
    //         if (!IsValidTimestamp(epoch)) epoch = previous;
    //         frame->time.pts_us -= epoch;
    //         frame->time.dts_us = frame->time.pts_us;
    //         std::shared_ptr<MediaFrame> converted;
    //         Require(converter.Convert(*frame, converted), "Capture frame conversion failed");
    //         std::vector<PacketPtr> packets;
    //         Require(encoder.Encode(converted, packets), "Capture frame encoding failed");
    //         write_packets(packets);
    //         if (++frames >= 12) session.RequestStop();
    //         cv.notify_all();
    //     });
    //     const auto opened = session.Start();
    //     if (opened.error) throw std::runtime_error(opened.error->message);
    //     {
    //         std::unique_lock<std::mutex> lock(mutex);
    //         const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    //         while (frames.load() < 12 && session.GetState() == VideoCaptureSession::State::Running &&
    //             std::chrono::steady_clock::now() < deadline) {
    //             cv.wait_for(lock, std::chrono::milliseconds(100));
    //         }
    //     }
    //     session.Stop();
    //     if (const auto error = session.LastError()) throw std::runtime_error(error->message);
    //     Require(frames.load() == 12 && session.FramesReceived() == 12, "Wrong session frame count");
    //     std::vector<PacketPtr> tail;
    //     Require(encoder.Flush(tail), "Encoder flush failed");
    //     write_packets(tail);
    //     Require(publisher.Close().Succeed() && packet_count == 12, "Wrong H264 output count or MP4 close failed");
    //     std::cout << "Native capture -> session -> I420 -> H264 -> Publisher/MP4: " << frames.load()
    //         << " frames, " << packet_count << " packets. Output: " << argv[3] << '\n';
    //     return 0;
    // } catch (const std::exception& error) {
    //     std::cerr << error.what() << '\n';
    //     return 1;
    // }
}