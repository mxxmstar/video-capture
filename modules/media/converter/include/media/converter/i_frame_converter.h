#pragma once
#include "media/media_frame.h"
#include <memory>
/// @brief 视频转换配置
/// @param width 目标宽度
/// @param height 目标高度
/// @param pixel_format 目标像素格式
struct VideoConvertConfig {
    int width{0};
    int height{0};
    PixelFormat pixel_format{PixelFormat::kUnknown};
    int sws_flags{2}; // Legacy generic tuning hint; hardware backends may ignore it.
};

/// @brief 音频转换配置
/// @param sample_rate 目标采样率
/// @param channels 目标通道数
/// @param channel_layout 目标通道布局
/// @param sample_format 目标样本格式
struct AudioConvertConfig {
    int sample_rate{0};
    int channels{0};
    uint64_t channel_layout{0};
    SampleFormat sample_format{SampleFormat::Unknown};
};

/// @brief 媒体帧转换配置
/// @param backend 转换后端
/// @param video 视频转换配置
/// @param audio 音频转换配置
struct MediaFrameConverterConfig {
    VideoConvertConfig video;
    AudioConvertConfig audio;
};

class IFrameConverter {
public:
 virtual ~IFrameConverter() = default;
 virtual bool Open(const MediaFrameConverterConfig&) = 0;
 virtual bool Convert(const MediaFrame&, std::shared_ptr<MediaFrame>&) = 0;
 virtual void Close() = 0;
};
