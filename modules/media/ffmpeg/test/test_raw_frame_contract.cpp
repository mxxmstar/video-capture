/// @file test_raw_frame_contract.cpp
/// @brief 验证 raw AVFrame 的所有权、平面访问和引用生命周期契约。

#include "media/generic/ffmpeg_frame_owner.h"
#include "media/generic/ffmpeg_frame_view.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "raw frame contract: " << message << std::endl;
    }
    return condition;
}

AVFrame* MakeVideoFrame(AVPixelFormat format, int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    for (int plane = 0; plane < 4; ++plane) {
        if (frame->data[plane]) {
            frame->data[plane][0] = static_cast<std::uint8_t>(0x20 + plane);
        }
    }
    return frame;
}

AVFrame* MakeAudioFrame(AVSampleFormat format, int channels, int samples) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = format;
    frame->sample_rate = 48000;
    frame->nb_samples = samples;
    av_channel_layout_default(&frame->ch_layout, channels);
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

bool CheckVideo(AVPixelFormat format, int expected_planes) {
    auto owner = Media::OwnAVFrame(MakeVideoFrame(format, 16, 8));
    if (!Check(owner != nullptr, "video allocation")) return false;
    auto view = Media::MakeFFmpegFrameView(*owner, owner);
    if (!Check(view.PlaneCount() == static_cast<size_t>(expected_planes), "video plane count")) return false;
    for (int p = 0; p < expected_planes; ++p) {
        const auto* plane = view.Plane(p);
        if (!Check(plane && plane->IsValid() && plane->data[0] == 0x20 + p,
                   "video plane address and geometry")) return false;
    }
    if (!Check(view.Plane(expected_planes) == nullptr, "out of range plane rejected")) return false;
    auto referenced = Media::OwnAVFrame(av_frame_alloc());
    if (!Check(referenced && av_frame_ref(referenced.get(), owner.get()) == 0,
               "AVFrame reference succeeds")) return false;
    if (!Check(referenced->data[0] == owner->data[0], "reference shares video payload")) return false;
    std::weak_ptr<AVFrame> weak = owner;
    owner.reset();
    if (!Check(!weak.expired() && view.Plane(0)->data[0] == 0x20, "view retains AVFrame")) return false;
    view = {};
    return Check(weak.expired() && referenced->data[0][0] == 0x20,
                 "AVFrame reference survives view release");
}

bool CheckAudio() {
    auto owner = Media::OwnAVFrame(MakeAudioFrame(AV_SAMPLE_FMT_FLTP, 2, 32));
    if (!Check(owner != nullptr, "audio allocation")) return false;
    auto view = Media::MakeFFmpegFrameView(*owner, owner);
    if (!Check(view.PlaneCount() == 2, "audio plane count")) return false;
    for (size_t p = 0; p < 2; ++p) {
        if (!Check(view.Plane(p) && view.Plane(p)->IsValid() &&
                   view.PlaneSize(p) == 32 * sizeof(float), "audio plane geometry")) return false;
    }
    return true;
}

}  // namespace

int main() {
    const bool passed = CheckVideo(AV_PIX_FMT_YUV420P, 3) &&
                        CheckVideo(AV_PIX_FMT_NV12, 2) &&
                        CheckAudio();
    if (passed) {
        std::cout << "raw AVFrame contract test passed" << std::endl;
        return 0;
    }
    return 1;
}
