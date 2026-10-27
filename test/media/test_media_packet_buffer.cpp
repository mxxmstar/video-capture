#include "media/media_packet_buffer.h"
#include "media/ffmpeg_packet_utils.h"
#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/decoder/ffmpeg_decoder.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void TestBytesAndOwnership() {
    uint8_t source[]{1,2,3,4};
    MediaPacketBuffer copied(source, sizeof(source));
    source[0] = 9;
    Require(copied.Size() == 4 && copied.Bytes()[0] == 1, "input was not copied");
    MediaPacketBuffer shared = copied;
    copied = {};
    Require(shared.Data()[3] == 4, "copy did not retain bytes");
    MediaPacketBuffer moved(std::move(shared));
    Require(shared.Empty() && shared.Data() == nullptr && moved.Size() == 4, "move constructor contract");
    shared = std::move(moved);
    Require(moved.Empty() && shared.Data()[0] == 1, "move assignment contract");
    MediaPacketBuffer empty(nullptr, 0);
    Require(empty.Empty() && empty.Bytes().empty(), "empty payload contract");
    bool rejected = false;
    try { MediaPacketBuffer invalid(nullptr, 1); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "null nonempty payload accepted");
    rejected = false;
    try { MediaPacketBuffer::Wrap(source, 4, {}); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "unowned external payload accepted");
    auto owner = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{5,6,7});
    std::weak_ptr<const std::vector<uint8_t>> weak = owner;
    auto external = MediaPacketBuffer::Wrap(owner->data(), owner->size(), owner);
    const auto* address = owner->data();
    owner.reset();
    Require(!weak.expired() && external->Data() == address && external->Data()[1] == 6, "zero-copy owner lost");
    external.reset();
    Require(weak.expired(), "external owner leaked");
    static_assert(std::is_same_v<decltype(shared.Data()), const uint8_t*>);
}
void TestPacketReferences() {
    auto owner = Media::OwnAVPacket(av_packet_alloc());
    Require(owner && av_new_packet(owner.get(), 4) == 0, "allocate packet");
    owner->data[0] = 42;
    auto* side = av_packet_new_side_data(owner.get(), AV_PKT_DATA_STRINGS_METADATA, 4);
    Require(side != nullptr, "allocate packet side data");
    std::memcpy(side, "abc", 4);
    MediaPacket packet;
    Media::BindAVPacket(packet, owner);
    Require(Media::GetBackendPacket(packet) == owner.get(), "backend binding mismatch");
    auto reference = Media::OwnAVPacket(av_packet_clone(owner.get()));
    Require(reference && reference->data == owner->data && reference->side_data_elems == 1 &&
            reference->side_data[0].data[0] == 'a', "reference lost payload or side data");
    MediaPacket queued = packet;
    std::weak_ptr<AVPacket> weak = owner;
    owner.reset(); packet = {};
    Require(!weak.expired() && queued.buffer->Data()[0] == 42, "queued copy lost AVPacket");
    queued.buffer = std::make_shared<MediaPacketBuffer>(std::vector<uint8_t>{0,0,0,0});
    Require(weak.expired() && Media::GetBackendPacket(queued) == nullptr, "stale backend was dereferenced");
    queued = {};
    Require(reference->data[0] == 42 && reference->side_data[0].data[0] == 'a', "AVPacket reference lifetime");
}
void TestEncodeDecode() {
    EncoderConfig config;
    config.encoder_name = "libx264";
    config.global_header = true;
    config.video().width = config.video().height = 16;
    config.video().fps_num = 25;
    FFmpegEncoder encoder;
    Require(encoder.Open(config), "open H264 encoder");
    const auto track = encoder.GetOutputInfo();
    std::vector<PacketPtr> packets;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> pixels(16*16*3/2, 128);
        std::fill_n(pixels.begin(),16*16,static_cast<uint8_t>(40+i*20));
        auto frame = MediaFrame::CreateCompact(pixels.data(),pixels.size(),PixelFormat::kI420,16,16,16);
        Require(frame != nullptr, "create input frame");
        frame->time = {i*40000,i*40000,40000};
        std::vector<PacketPtr> output;
        Require(encoder.Encode(frame, output), "encode frame");
        packets.insert(packets.end(),output.begin(),output.end());
    }
    std::vector<PacketPtr> tail;
    Require(encoder.Flush(tail), "flush encoder");
    packets.insert(packets.end(),tail.begin(),tail.end());
    Require(packets.size() == 3, "encoded packet count");
    encoder.Close();
    MediaStreamInfo info;
    info.media_type = MediaType::VIDEO;
    info.codec_type = track.codec_type;
    info.time_base = track.time_base;
    info.extra_data = track.extra_data;
    info.detail = VideoStreamInfo{16,16,25,PixelFormat::kI420};
    for (bool copy_payload : {false,true}) {
        FFmpegDecoder decoder;
        Require(decoder.Open(info), "open decoder");
        std::vector<FramePtr> frames;
        decoder.SetFrameCallback([&](FramePtr frame) { frames.push_back(std::move(frame)); });
        for (const auto& encoded : packets) {
            Require(Media::GetBackendPacket(*encoded) != nullptr, "encoder packet owner invalid");
            auto input = std::make_shared<MediaPacket>(*encoded);
            if (copy_payload) {
                input->buffer = std::make_shared<MediaPacketBuffer>(encoded->buffer->Data(),encoded->buffer->Size());
                input->backend = {};
            }
            Require(decoder.Decode(input), "decode packet");
        }
        Require(decoder.Flush(), "flush decoder");
        decoder.Close();
        Require(frames.size() == 3, "decoded frame count");
        for (size_t i = 0; i < frames.size(); ++i) {
            Require(frames[i]->Width() == 16 && frames[i]->PlaneCount() == 3 &&
                    frames[i]->PlaneView(0).IsValid(), "decoded frame view after close");
            Require(frames[i]->time.pts_us == static_cast<int64_t>(i)*40000, "decode timestamps");
#if RAW_FRAME_BUFFER
            Require(frames[i]->backend.type == BackendHandle::FFMPEG &&
                    frames[i]->view.Owner().get() == frames[i]->backend.ptr, "raw frame owner binding");
#else
            Require(frames[i]->backend.type == BackendHandle::NONE && frames[i]->view.IsContiguous(),
                    "copy mode did not produce compact owned frame");
#endif
        }
    }
}
void TestAudioDecode() {
    MediaStreamInfo info;
    info.media_type = MediaType::AUDIO;
    info.codec_type = CodecType::G711A;
    info.time_base = {1,8000};
    info.detail = AudioStreamInfo{8000,1,AV_CH_LAYOUT_MONO,SampleFormat::S16};
    FFmpegDecoder decoder;
    Require(decoder.Open(info), "open G711 audio decoder");
    std::vector<FramePtr> frames;
    decoder.SetFrameCallback([&](FramePtr frame) { frames.push_back(std::move(frame)); });
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::AUDIO;
    packet->codec = CodecType::G711A;
    packet->pts = packet->dts = 0;
    packet->time_base = info.time_base;
    packet->buffer = std::make_shared<MediaPacketBuffer>(std::vector<uint8_t>(160,0xd5));
    Require(decoder.Decode(packet) && decoder.Flush(), "decode plain G711 bytes");
    packet.reset();
    decoder.Close();
    Require(frames.size() == 1 && frames[0]->NbSamples() == 160 && frames[0]->Channels() == 1 &&
            frames[0]->PlaneCount() == 1 && frames[0]->PlaneView(0).IsValid(), "decoded audio ownership and layout");
}

}
int main() {
    try {
        TestBytesAndOwnership(); TestPacketReferences(); TestEncodeDecode(); TestAudioDecode();
        std::cout << "MediaPacketBuffer ownership and encode/decode contracts passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
