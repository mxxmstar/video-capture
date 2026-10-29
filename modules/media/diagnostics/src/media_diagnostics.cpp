#include "common/log/logger.h"
#include "media/media_packet.h"
#include "media/stream/stream_info.h"

void MediaPacket::Dump() const {
        LOG_INFO("MediaPacket: type={}, codec={}, stream_index={}, pts={}, dts={}, duration={}, time_base={}, keyframe={}, buffer_size={}",
                 (int)type, (int)codec, stream_index, pts, dts, duration,
                 time_base.toString(), keyframe, buffer ? buffer->Size() : 0);
    }

void VideoStreamInfo::Dump() const {
        LOG_INFO("VideoStreamInfo: {}x{}, fps: {}, pixel_format: {}",
                 width, height, fps, static_cast<int>(pixel_format));
    }

void AudioStreamInfo::Dump() const {
        LOG_INFO("AudioStreamInfo: sample_rate: {}, channels: {}, channel_layout: {}, sample_format: {}",
                 sample_rate, channels, channel_layout, static_cast<int>(sample_format));
    }

void MediaStreamInfo::Dump(bool dump_extra_data) const {
        LOG_INFO("StreamInfo: {}", media_type == MediaType::VIDEO ? "VIDEO" : "AUDIO");
        LOG_INFO("stream_index: {}, codec_type: {}, time_base: {}",
                 stream_index, static_cast<int>(codec_type), time_base.toString());

        if (media_type == MediaType::VIDEO) {
            auto video_info = video();
            video_info.Dump();
        } else {
            auto audio_info = audio();
            audio_info.Dump();
        }

        if (dump_extra_data) {
            if (!extra_data.empty()) {
                std::string hex;
                for (size_t i = 0; i < extra_data.size() && i < 32; ++i) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02x ", extra_data[i]);
                    hex += buf;
                }
                LOG_INFO("extra_data ({} bytes): {}", extra_data.size(), hex);
            } else {
                LOG_INFO("extra_data: empty");
            }
        }
    }

#include "media/encoder/encoder_config.h"

void VideoEncoderConfig::Dump() const {
    LOG_INFO("VideoEncoderConfig: width={}, height={}, fps={}/{}, pixel_format={}, gop_size={}, max_b_frames={}, max_bitrate={}, rc_buffer_size={}, sws_flags={}, preset={}, tune={}, crf={}",
                width, height, fps_num, fps_den, static_cast<int>(pixel_format), gop_size, max_b_frames, max_bitrate, rc_buffer_size, sws_flags, preset, tune, crf);
}

void AudioEncoderConfig::Dump() const {
    LOG_INFO("AudioEncoderConfig: sample_rate={}, channels={}, channel_layout={}, sample_format={}",
                sample_rate, channels, channel_layout, static_cast<int>(sample_format));
}

#include "media/pusher/pusher_config.h"

#include "media/publisher/publisher_config.h"
