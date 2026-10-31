#include "media/pusher/pusher_factory.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
}
namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
void Check(int code, const char* operation) {
    if (code >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
void Check(const PusherResult& result) {
    if (!result.Succeed()) throw std::runtime_error(result.error->message);
}
}
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: test_mp4_rtsp_pusher INPUT.mp4 rtsp://HOST:PORT/APP/STREAM [seconds]\n";
        return 2;
    }
    if (std::string(argv[2]).rfind("rtsp://", 0) != 0) {
        std::cerr << "Expected rtsp:// output\n"; return 2;
    }
    double limit = 0;
    if (argc == 4) {
        try {
            std::size_t used = 0;
            std::string value = argv[3];
            limit = std::stod(value, &used);
            if (used != value.size() || !(limit > 0) || limit > 86400)
                throw std::invalid_argument("seconds");
        } catch (...) { std::cerr << "seconds must be in (0,86400]\n"; return 2; }
    }
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    AVFormatContext* input = nullptr;
    AVPacket* packet = nullptr;
    auto pusher = CreateMediaPusher();
    int status = 0;
    try {
        Check(avformat_network_init(), "network init");
        Check(avformat_open_input(&input, argv[1], nullptr, nullptr), "open MP4");
        Check(avformat_find_stream_info(input, nullptr), "read stream info");
        int selected = -1;
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                input->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264) {
                selected = static_cast<int>(i); break;
            }
        }
        if (selected < 0) throw std::runtime_error("Current IPusher requires H264 video; no transcoding");
        auto* stream = input->streams[selected];
        const auto* codec = stream->codecpar;
        const auto fps = av_guess_frame_rate(input, stream, nullptr);
        if (fps.num <= 0 || fps.den <= 0) throw std::runtime_error("Missing video frame rate");
        PusherConfig config;
        config.output_url = argv[2];
        config.ffmpeg.rtsp = RtspOutputOptions{"tcp"};
        auto& track = config.video_track;
        track.track_id = selected;
        track.codec_type = CodecType::H264;
        track.media_type = MediaType::VIDEO;
        track.video().width = codec->width;
        track.video().height = codec->height;
        track.video().fps = static_cast<float>(av_q2d(fps));
        track.time_base_num = stream->time_base.num;
        track.time_base_den = stream->time_base.den;
        if (codec->extradata_size > 0)
            track.extra_data.assign(codec->extradata, codec->extradata + codec->extradata_size);
        Check(pusher->Open(config));
        packet = av_packet_alloc();
        if (!packet) throw std::bad_alloc();
        const auto start = Clock::now();
        int64_t origin = AV_NOPTS_VALUE, report = -1;
        unsigned long long count = 0;
        while (!stopped) {
            const int result = av_read_frame(input, packet);
            if (result == AVERROR_EOF) break;
            Check(result, "read MP4 packet");
            if (packet->stream_index != selected) { av_packet_unref(packet); continue; }
            const int64_t stamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (stamp == AV_NOPTS_VALUE) throw std::runtime_error("Video packet lacks DTS/PTS");
            if (origin == AV_NOPTS_VALUE) origin = stamp;
            const auto elapsed = av_rescale_q(stamp - origin, stream->time_base, AV_TIME_BASE_Q);
            if (limit > 0 && elapsed >= limit * AV_TIME_BASE) break;
            const auto due = start + std::chrono::microseconds(elapsed);
            while (!stopped && Clock::now() < due) {
                if (limit > 0 && std::chrono::duration<double>(Clock::now()-start).count() >= limit) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (stopped || (limit > 0 && std::chrono::duration<double>(Clock::now()-start).count() >= limit)) break;
            MediaPacket output;
            output.type = MediaType::VIDEO;
            output.codec = CodecType::H264;
            output.stream_index = selected;
            output.time_base = {stream->time_base.num, stream->time_base.den};
            output.pts = packet->pts == AV_NOPTS_VALUE ? kNoTimestamp : packet->pts - origin;
            output.dts = packet->dts == AV_NOPTS_VALUE ? kNoTimestamp : packet->dts - origin;
            output.duration = packet->duration;
            output.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            // Exercise the plain-byte MediaPacket path also used by future MPP encoders.
            // The test owns its payload; IPusher must not consume/mutate caller storage.
            output.buffer = std::make_shared<MediaPacketBuffer>(packet->data, packet->size);
            Check(pusher->Push(output));
            av_packet_unref(packet);
            ++count;
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-start).count();
            if (seconds / 5 != report) {
                report = seconds / 5;
                std::cout << "IPusher elapsed=" << seconds << "s packets=" << count << std::endl;
            }
        }
        if (!count && !stopped) throw std::runtime_error("No video packets published");
        Check(pusher->Close());
        std::cout << "IPusher finished packets=" << count << std::endl;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl; status = 1;
    }
    pusher->Close();
    av_packet_free(&packet);
    avformat_close_input(&input);
    avformat_network_deinit();
    return status;
}
