#pragma once
#include <memory>
extern "C" {
#include <libavutil/frame.h>
}
namespace Media {
struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
inline std::shared_ptr<AVFrame> OwnAVFrame(AVFrame* frame) {
    return std::shared_ptr<AVFrame>(frame, AVFrameDeleter{});
}
} // namespace Media
