#include "media/media_frame.h"
#include "media/converter/generic/media_frame_converter.h"
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct OwnedAVFrame {
    AVFrame* value = av_frame_alloc();
    ~OwnedAVFrame() { av_frame_free(&value); }
};
void TestIndependentVideoPlanes() {
    const uint8_t y[]{10,11,12,13,99,99,20,21,22,23};
    const uint8_t uv[]{30,31,32,33};
    auto owner = std::make_shared<MediaBuffer>();
    owner->PushPlane(y, sizeof(y), PlaneGeometry{6,4,2});
    owner->PushPlane(uv, sizeof(uv), PlaneGeometry{4,4,1});
    std::weak_ptr<MediaBuffer> weak = owner;
    MediaFrame frame;
    frame.meta = VideoFrameMeta{PixelFormat::kNV12,4,2};
    frame.view = MediaBufferView::FromOwner(owner);
    owner.reset();
    Require(!weak.expired() && frame.PlaneCount() == 2 && frame.Stride(0) == 6, "view-only ownership or geometry");
    MediaFrame retained = frame;
    frame.view = {};
    Require(retained.PlaneView(0).data[6] == 20, "copied frame lost owner");
    OwnedAVFrame output;
    Require(MediaFrameConverter::MediaFrameToAVFrame(retained, output.value), "independent NV12 conversion");
    Require(output.value->data[0][output.value->linesize[0]] == 20 && output.value->data[1][0] == 30, "independent NV12 pixels");
    retained.view = MediaBufferView::FromPlanes({MediaPlaneView(y, 9, PlaneGeometry{6,4,2}), MediaPlaneView(uv,4,PlaneGeometry{4,4,1})});
    Require(!retained.PlaneView(0).data, "truncated plane accepted");
    Require(!MediaFrameConverter::MediaFrameToAVFrame(retained, output.value), "truncated video converted");
    retained.view = MediaBufferView::FromPlanes({MediaPlaneView(y,10,PlaneGeometry{6,4,2})});
    Require(!MediaFrameConverter::MediaFrameToAVFrame(retained, output.value), "missing UV plane accepted");
}
void TestAudioPlanes() {
    const uint8_t left[]{1,2,3,4}, right[]{5,6,7,8};
    auto owner = std::make_shared<MediaBuffer>();
    owner->Push(left,4,PlaneGeometry{4,4,1});
    owner->Push(right,4,PlaneGeometry{4,4,1});
    MediaFrame frame;
    frame.type = MediaType::AUDIO;
    AudioFrameMeta meta;
    meta.sample_format = SampleFormat::S16P;
    meta.sample_rate = 48000; meta.channels = 2; meta.nb_samples = 2;
    frame.meta = meta;
    frame.view = MediaBufferView::FromOwner(owner);
    OwnedAVFrame output;
    Require(MediaFrameConverter::MediaFrameToAVFrame(frame, output.value), "planar audio conversion");
    Require(output.value->extended_data[0][0] == 1 && output.value->extended_data[1][0] == 5, "audio plane pixels");
    MediaFrame packed;
    Require(MediaFrameConverter::AvFrameToMediaFrame(*output.value, &packed), "audio output view creation");
    Require(packed.PlaneCount() == 2 && packed.PlaneView(1).data[0] == 5, "audio output view ownership");
}

void TestStorageConversions() {
    const uint8_t y[]{10,11,12,13,99,99,20,21,22,23};
    const uint8_t uv[]{30,31,32,33};
    auto separate = std::make_shared<MediaBuffer>();
    separate->PushPlane(y, sizeof(y), PlaneGeometry{6,4,2});
    separate->PushPlane(uv, sizeof(uv), PlaneGeometry{4,4,1});
    auto separate_view = MediaBufferView::FromOwner(separate);
    Require(!separate->IsContiguous() && !separate_view.IsContiguous(), "separate planes reported contiguous");
    Require(separate->MemoryRegions().size() == 2 && separate_view.MemoryRegions().size() == 2,
        "separate storage regions are not exposed");

    auto contiguous = MediaBuffer::CopyContiguous(separate_view);
    Require(contiguous && contiguous->IsContiguous() && contiguous->PlaneCount() == 2,
        "contiguous copy lost logical planes");
    const auto bytes = contiguous->ContiguousBytes();
    Require(bytes && bytes->size() == 12 && (*bytes)[4] == 20 && (*bytes)[8] == 30,
        "contiguous copy did not remove row padding");
    auto contiguous_view = MediaBufferView::FromOwner(contiguous);
    Require(contiguous_view.IsContiguous() && contiguous_view.PlaneCount() == 2,
        "multi-plane contiguous view was misclassified");
    Require(contiguous->MemoryRegions().size() == 1 && contiguous_view.MemoryRegions().size() == 1,
        "contiguous backing region was not exposed once");

    auto appended = MediaBuffer::CopyContiguous(separate_view);
    const auto* appended_source = appended->Data(1);
    appended->PushPlane(appended_source, 4, PlaneGeometry{4,4,1});
    Require(!appended->IsContiguous() && appended->PlaneCount() == 3 &&
        appended->MemoryRegions().size() == 3 && appended->Data(0)[4] == 20 &&
        appended->Data(1)[0] == 30 && appended->Data(2)[0] == 30,
        "appending an aliased plane lost separate storage or existing pixels");
    MediaBuffer moved(std::move(*appended));
    *appended = std::move(moved);
    Require(appended->Data(0)[4] == 20 && appended->Data(2)[0] == 30,
        "moving owned storage invalidated plane addresses");
    auto appended_view = MediaBufferView::FromOwner(appended);
    auto repacked = MediaBuffer::CopyContiguous(appended_view);
    Require(repacked && repacked->PlaneCount() == 3 && repacked->TotalSize() == 16 &&
        repacked->Data(2) == repacked->Data(0) + 12 && repacked->Data(2)[0] == 30,
        "repacking appended separate storage lost offsets");

    auto split = MediaBuffer::CopySeparatePlanes(contiguous_view);
    Require(split && !split->IsContiguous() && split->PlaneCount() == 2,
        "separate-plane copy was misclassified");
    Require(split->MutableRegions().size() == 2 && split->Data(1)[0] == 30,
        "separate-plane regions are not accessible");
}

void TestRawAndNegativeStride() {
    AVFrame* raw = av_frame_alloc();
    Require(raw != nullptr, "allocate raw frame");
    raw->format = AV_PIX_FMT_GRAY8; raw->width = 4; raw->height = 2;
    Require(av_frame_get_buffer(raw,32) >= 0, "allocate raw pixels");
    raw->data[0][0] = 10; raw->data[0][raw->linesize[0]] = 20;
    raw->data[0] += raw->linesize[0]; raw->linesize[0] = -raw->linesize[0];
    std::shared_ptr<MediaFrame> frame;
    Require(MediaFrameConverter::AdoptAVFrame(raw,frame), "raw adoption");
    OwnedAVFrame fast_output;
    Require(MediaFrameConverter::MediaFrameToAVFrame(*frame, fast_output.value), "raw fast path");
    Require(fast_output.value->data[0] == raw->data[0], "raw fast path copied planes");
    MediaFrame truncated = *frame;
    auto invalid_planes = frame->view.Planes();
    invalid_planes[0].size = 1;
    truncated.view = MediaBufferView(std::move(invalid_planes));
    Require(!MediaFrameConverter::MediaFrameToAVFrame(truncated, fast_output.value), "backend bypassed invalid view");
    truncated = {};
    // The non-owning backend pointer may remain: view alone keeps its owner alive.
    const auto plane = frame->PlaneView(0);
    Require(plane.data && plane.geometry.stride < 0 && plane.data[0] == 20 && plane.data[plane.geometry.stride] == 10, "negative stride geometry");
    OwnedAVFrame output;
    Require(MediaFrameConverter::MediaFrameToAVFrame(*frame, output.value), "negative stride conversion");
    Require(output.value->data[0][0] == 20 && output.value->data[0][output.value->linesize[0]] == 10, "negative stride pixels");
    MediaFrame packed;
    Require(MediaFrameConverter::AvFrameToMediaFrame(*output.value,&packed), "packed video view creation");
    Require(packed.PlaneView(0).data[4] == 10, "packed video owner");
}
}
int main() {
    try {
        TestIndependentVideoPlanes(); TestAudioPlanes(); TestStorageConversions(); TestRawAndNegativeStride();
        std::cout << "MediaFrame view contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
