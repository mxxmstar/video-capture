#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <variant>

#include "media/media_packet.h"
#include "media/media_frame.h"


/// @brief 视频流信息
struct VideoStreamInfo {
    int width{0};   ///< 视频宽度
    int height{0};  ///< 视频高度
    float fps{0};   ///< 视频帧率
    PixelFormat pixel_format{PixelFormat::kUnknown}; ///< 视频像素格式
    void Dump() const;
};

/// @brief 音频流信息
struct AudioStreamInfo {
    int sample_rate{0}; ///< 音频采样率
    int channels{0};   ///< 音频通道数
    uint64_t channel_layout{0}; ///< 音频通道布局
    SampleFormat sample_format{SampleFormat::Unknown}; ///< 音频采样格式
    void Dump() const;
};
/// @brief 媒体流信息
///
/// 描述一路媒体流的编码参数与元数据。
/// 由 IPuller 在连接成功后构造，传递 StreamSession -> MediaStreamSource。
struct MediaStreamInfo {
    MediaType media_type   = MediaType::UNKNOWN; ///< 媒体类型（视频/音频）
    CodecType codec_type   = CodecType::UNKNOWN; ///< 编码格式（H264/H265/AAC/…）
    int       stream_index = -1;                  ///< 流索引
    Rational time_base{1, 1000000}; ///< 时间基（微秒/单位）
    std::vector<uint8_t> extra_data; ///< 额外数据（如 H.264 SPS/PPS）
    std::variant<VideoStreamInfo, AudioStreamInfo> detail;

    /// @brief 获取指定类型的详细信息
    template<typename T>
    const T& get_detail() const { return std::get<T>(detail); }

    VideoStreamInfo video() const {
        if (media_type == MediaType::VIDEO) {
            return std::get<VideoStreamInfo>(detail);
        }
        return VideoStreamInfo{};
    }
    AudioStreamInfo audio() const {
        if (media_type == MediaType::AUDIO) {
            return std::get<AudioStreamInfo>(detail);
        }
        return AudioStreamInfo{};
    }

    /// @brief 打印流信息到日志
    void Dump(bool dump_extra_data = true) const;
};

struct MultiStreamInfo {
    std::vector<MediaStreamInfo> stream_infos;
    int video_stream_idx_{-1}; ///< index into stream_infos, not FFmpeg stream_index
    int audio_stream_idx_{-1}; ///< index into stream_infos, not FFmpeg stream_index

    bool HasVideoStream() const {
        return video_stream_idx_ >= 0 &&
               video_stream_idx_ < static_cast<int>(stream_infos.size());
    }
    bool HasAudioStream() const {
        return audio_stream_idx_ >= 0 &&
               audio_stream_idx_ < static_cast<int>(stream_infos.size());
    }

    void DumpStreamInfo() {
        if (HasVideoStream()) {
            stream_infos[video_stream_idx_].Dump();
        }
        if (HasAudioStream()) {
            stream_infos[audio_stream_idx_].Dump();
        }
    }
};
