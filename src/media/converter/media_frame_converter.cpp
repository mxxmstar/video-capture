#include "media/converter/media_frame_converter.h"

#include "media/converter/ffmpeg_audio_converter.h"
#include "media/converter/ffmpeg_video_converter.h"
#include "media/ffmpeg_frame_owner.h"
#include "media/ffmpeg_format.h"
#include "media/ffmpeg_frame_view.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

using namespace Media;

namespace {

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_make_error_string(buffer, sizeof(buffer), error_code);
    return buffer;
}

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// @brief 根据声道数量生成默认布局掩码
/// @param channels 声道数
/// @return 默认布局掩码
uint64_t defaultChannelLayoutMask(int channels) {
    AVChannelLayout layout{};
    if (channels <= 0) {
        return 0;
    }
    av_channel_layout_default(&layout, channels);
    const uint64_t mask = layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
    av_channel_layout_uninit(&layout);
    return mask;
}

/// @brief 安全地获取 AVFrame 指定平面（plane）的数据指针
/// @param frame AVFrame 引用
/// @param plane 平面索引
/// @return 数据指针，或 nullptr 如果平面索引无效或未分配数据
const uint8_t* framePlane(const AVFrame& frame, int plane) {
    if (plane < 0) {
        return nullptr;
    }
    // 优先使用 extended_data，音频声道数大于8时ffmpeg会使用 extended_data 来存储数据
    if (frame.extended_data) {
        return frame.extended_data[plane];
    }
    return plane < AV_NUM_DATA_POINTERS ? frame.data[plane] : nullptr;
}

/// @brief 从 AVFrame 填充 MediaFrame 的类型和元数据，不复制平面数据。
///
/// @param frame AVFrame 引用
/// @param media_type 输出的媒体类型
/// @param meta 输出的元数据
/// @param error 输出的错误信息
/// @return true 成功 false 失败
bool FillRawFrameMeta(const AVFrame& frame, MediaType& media_type, FrameMeta& meta, std::string& error) {
    if (frame.width > 0 && frame.height > 0) {
        const auto format = static_cast<AVPixelFormat>(frame.format);
        const PixelFormat pixel_format = FromAVPixelFormat(format);
        const int plane_count = av_pix_fmt_count_planes(format);
        if (pixel_format == PixelFormat::kUnknown || !frame.data[0] || plane_count <= 0 || plane_count > 4) {
            error = "Unsupported or empty video AVFrame";
            return false;
        }

        VideoFrameMeta video{};
        video.pixel_format = pixel_format;
        video.width = frame.width;
        video.height = frame.height;

        media_type = MediaType::VIDEO;
        meta = video;
        return true;
    }

    if (frame.nb_samples > 0 && frame.ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(frame.format);
        const SampleFormat sample_format = FromAVSampleFormat(format);
        const int channels = frame.ch_layout.nb_channels;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? channels : 1;
        if (sample_format == SampleFormat::Unknown || frame.sample_rate <= 0 ||
            bytes_per_sample <= 0 || plane_count <= 0 ||
            !frame.extended_data) {
            error = "Unsupported or empty audio AVFrame";
            return false;
        }

        AudioFrameMeta audio{};
        audio.sample_format = sample_format;
        audio.sample_rate = frame.sample_rate;
        audio.channels = channels;
        audio.channel_layout = frame.ch_layout.order == AV_CHANNEL_ORDER_NATIVE
            ? frame.ch_layout.u.mask : 0;
        audio.nb_samples = frame.nb_samples;
        audio.bytes_per_sample = bytes_per_sample;
        audio.planar = planar;
        for (int plane = 0; plane < plane_count; ++plane) {
            if (!frame.extended_data[plane]) {
                error = "Audio AVFrame plane is null";
                return false;
            }
        }

        media_type = MediaType::AUDIO;
        meta = audio;
        return true;
    }

    error = "AVFrame is neither a valid video nor audio frame";
    return false;
}


}  // namespace

void FFmpegAudioConverterDeleter::operator()(FFmpegAudioConverter* converter) const noexcept {
    delete converter;
}

void FFmpegVideoConverterDeleter::operator()(FFmpegVideoConverter* converter) const noexcept {
    delete converter;
}

MediaFrameConverter::MediaFrameConverter() = default;

MediaFrameConverter::~MediaFrameConverter() {
    Close();
}

bool MediaFrameConverter::Open(const MediaFrameConverterConfig& config) {
    Close();
    // 暂时只实现了 ffmpeg 的 sws、swr 转换功能
    if (config.backend != ConvertBackend::FFmpeg) {
        last_error_ = "Only FFmpeg converter backend is implemented";
        return false;
    }
    // 检查是否有有效配置
    const bool video_requested = config.video.width > 0 ||
        config.video.height > 0 || config.video.pixel_format != PixelFormat::kUnknown;
    const bool audio_requested = config.audio.sample_rate > 0 ||
        config.audio.channels > 0 || config.audio.channel_layout != 0 ||
        config.audio.sample_format != SampleFormat::Unknown;

    if (!video_requested && !audio_requested) {
        last_error_ = "At least one media conversion configuration is required";
        return false;
    }

    if (video_requested && (config.video.width <= 0 || config.video.height <= 0 ||
         ToAVPixelFormat(config.video.pixel_format) == AV_PIX_FMT_NONE)) {
        last_error_ = "Invalid video converter configuration";
        return false;
    }

    if (audio_requested && (config.audio.sample_rate <= 0 || config.audio.channels <= 0 ||
         ToAVSampleFormat(config.audio.sample_format) == AV_SAMPLE_FMT_NONE)) {
        last_error_ = "Invalid audio converter configuration";
        return false;
    }

    video_config_ = config.video;
    audio_config_ = config.audio;
    backend_ = config.backend;

    if (audio_requested && audio_config_.channel_layout == 0) {
        audio_config_.channel_layout = defaultChannelLayoutMask(audio_config_.channels);
    }
    if (audio_requested && audio_config_.channel_layout == 0) {
        last_error_ = "Unable to determine audio channel layout";
        return false;
    }

    if (video_requested) {
        ffmpeg_video_converter_.reset(new FFmpegVideoConverter());
        if (!ffmpeg_video_converter_->Open(video_config_.width, video_config_.height,
                ToAVPixelFormat(video_config_.pixel_format), video_config_.sws_flags)) {
            const std::string error = ffmpeg_video_converter_->LastError();
            Close();
            last_error_ = error;
            return false;
        }
    }

    if (audio_requested) {
        ffmpeg_audio_converter_.reset(new FFmpegAudioConverter());
        if (!ffmpeg_audio_converter_->Open( audio_config_.channel_layout,
                audio_config_.sample_rate, ToAVSampleFormat(audio_config_.sample_format))) {
            const std::string error = ffmpeg_audio_converter_->LastError();
            Close();
            last_error_ = error;
            return false;
        }
    }

    opened_ = true;
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::Convert(const MediaFrame& input, std::shared_ptr<MediaFrame>& output) {
    output.reset();

    if (!opened_) {
        last_error_ = "MediaFrameConverter is not opened";
#if CONVERT_STATS_ENABLE
        ++stats.convert_errors;
#endif
        return false;
    }

#if CONVERT_STATS_ENABLE
    int64_t convert_start_time = Now();
    ++stats.convert_calls;
#endif

    bool result = false;
    if (input.type == MediaType::VIDEO) {
        result = ffmpegVideoConvert(input, output);
    } else if (input.type == MediaType::AUDIO) {
        result = ffmpegAudioConvert(input, output);
    } else {
        last_error_ = "Unsupported MediaFrame type";
#if CONVERT_STATS_ENABLE
        ++stats.convert_errors;
#endif
        return false;
    }

#if CONVERT_STATS_ENABLE
    if (result) {
        ++stats.convert_frames;
        int64_t convert_time_us = Now() - convert_start_time;
        stats.total_convert_time_us += convert_time_us;
        if (convert_time_us > stats.max_convert_time_us) {
            stats.max_convert_time_us = convert_time_us;
        }
        if (convert_time_us < stats.min_convert_time_us) {
            stats.min_convert_time_us = convert_time_us;
        }
    } else {
        ++stats.convert_errors;
    }
#endif

    return result;
}

bool MediaFrameConverter::ffmpegVideoConvert(const MediaFrame& input,
    std::shared_ptr<MediaFrame>& output) {
    if (!ffmpeg_video_converter_) {
        last_error_ = "FFmpeg video converter is not configured";
        return false;
    }
    // 创建 AVFrame
    AVFrame* source = av_frame_alloc();
    if (!source) {
        last_error_ = "av_frame_alloc for video source failed";
        return false;
    }
    // 转换为 AVFrame
    if (!MediaFrameToAVFrame(input, source)) {
        av_frame_free(&source);
        return false;
    }
    // 转换
    AVFrame* converted = ffmpeg_video_converter_->Convert(source);
    av_frame_free(&source);
    if (!converted) {
        last_error_ = ffmpeg_video_converter_->LastError();
        return false;
    }

#if !RAW_FRAME_BUFFER
    // 创建 MediaFrame 并将转换后的 AVFrame 转换为 MediaFrame
    // 注意：MediaFrame.view 仍然负责保持原始 AVFrame 的生命周期，backend.ptr 只是指向该对象的非拥有指针。
    output = std::make_shared<MediaFrame>();
    if (!AvFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
#else
    // 转换器已经把数据写入 converted。这里直接把 AVFrame 交给输出
    // MediaFrame，避免再次打包到 owned storage。
    if (!AdoptAVFrame(converted, output)) {
        return false;
    }
#endif
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::ffmpegAudioConvert(const MediaFrame& input,
    std::shared_ptr<MediaFrame>& output) {
    if (!ffmpeg_audio_converter_) {
        last_error_ = "FFmpeg audio converter is not configured";
        return false;
    }

    AVFrame* source = av_frame_alloc();
    if (!source) {
        last_error_ = "av_frame_alloc for audio source failed";
        return false;
    }

    if (!MediaFrameToAVFrame(input, source)) {
        av_frame_free(&source);
        return false;
    }

    AVFrame* converted = ffmpeg_audio_converter_->Convert(source);
    av_frame_free(&source);
    if (!converted) {
        last_error_ = ffmpeg_audio_converter_->LastError();
        return false;
    }

#if !RAW_FRAME_BUFFER
    output = std::make_shared<MediaFrame>();
    if (!AvFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
#else
    // 音频和视频使用相同的所有权转移规则：输出 MediaFrame 的 buffer
    // 持有 AVFrame，backend.ptr 指向同一个 AVFrame 但不单独负责释放。
    if (!AdoptAVFrame(converted, output)) {
        return false;
    }
#endif
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::MediaFrameToAVFrame(const MediaFrame& input, AVFrame* av_frame) {
    if (!av_frame) {
        last_error_ = "Output AVFrame is null";
        return false;
    }

    if (input.backend.type == BackendHandle::FFMPEG && input.backend.ptr && input.view.Owner().get() == input.backend.ptr) {
        const auto* source = static_cast<const AVFrame*>(input.backend.ptr);
        bool matches_view = false;
        if (input.view.PlaneCount() != 0) {
            const auto original = MakeFFmpegFrameView(*source, input.view.Owner());
            matches_view = original.PlaneCount() == input.view.PlaneCount();
            if (const auto* video = input.VideoMeta()) {
                matches_view = matches_view && video->width == source->width && video->height == source->height &&
                    video->pixel_format == FromAVPixelFormat(static_cast<AVPixelFormat>(source->format));
            } else if (const auto* audio = input.AudioMeta()) {
                matches_view = matches_view && audio->nb_samples == source->nb_samples &&
                    audio->sample_rate == source->sample_rate && audio->channels == source->ch_layout.nb_channels &&
                    audio->sample_format == FromAVSampleFormat(static_cast<AVSampleFormat>(source->format));
            }
            for (size_t p = 0; matches_view && p < original.PlaneCount(); ++p) {
                const auto& expected = *original.Plane(p);
                const auto& actual = *input.view.Plane(p);
                matches_view = actual.IsValid() && actual.data == expected.data && actual.size == expected.size &&
                    actual.geometry.stride == expected.geometry.stride && actual.geometry.rows == expected.geometry.rows &&
                    actual.geometry.row_bytes == expected.geometry.row_bytes;
            }
        }
        if (matches_view) {
            av_frame_unref(av_frame);
            const int ret = av_frame_ref(av_frame, source);
            if (ret < 0) {
                last_error_ = "av_frame_ref failed: " + AvErrorString(ret);
                return false;
            }
            SetAVFrameTime(input, av_frame);
            return true;
        }
    }

    if (input.view.PlaneCount() == 0) {
        last_error_ = "Input MediaFrame buffer is empty";
        return false;
    }

    if (input.type == MediaType::VIDEO) {
        const auto* meta = input.VideoMeta();
        if (!meta) {
            last_error_ = "Video MediaFrame has no metadata";
            return false;
        }

        const AVPixelFormat format = ToAVPixelFormat(meta->pixel_format);
        if (format == AV_PIX_FMT_NONE || meta->width <= 0 ||
            meta->height <= 0) {
            last_error_ = "Invalid video MediaFrame metadata";
            return false;
        }

        const int plane_count = av_pix_fmt_count_planes(format);
        if (plane_count <= 0 || plane_count > 4 ||
            input.view.PlaneCount() != static_cast<std::size_t>(plane_count)) {
            last_error_ = "Unsupported or incomplete video plane metadata";
            return false;
        }

        av_frame_unref(av_frame);
        av_frame->format = format;
        av_frame->width = meta->width;
        av_frame->height = meta->height;
        int ret = av_frame_get_buffer(av_frame, 32);
        if (ret < 0) {
            last_error_ = "av_frame_get_buffer for video failed: " +
                          AvErrorString(ret);
            return false;
        }

        const uint8_t* source_data[4]{};
        int source_linesize[4]{};
        int expected_row_bytes[4]{};
        ptrdiff_t compact_strides[4]{};
        size_t compact_sizes[4]{};
        if (av_image_fill_linesizes(expected_row_bytes, format, meta->width) < 0) return false;
        for (int p = 0; p < 4; ++p) compact_strides[p] = expected_row_bytes[p];
        if (av_image_fill_plane_sizes(compact_sizes, format, meta->height, compact_strides) < 0) return false;
        for (int plane = 0; plane < plane_count; ++plane) {
            const auto slice = input.PlaneView(plane);
            const auto expected_rows = compact_sizes[plane] / static_cast<size_t>(expected_row_bytes[plane]);
            const auto stride_magnitude = slice.geometry.stride < 0 ? static_cast<std::size_t>(-(slice.geometry.stride + 1)) + 1 : static_cast<std::size_t>(slice.geometry.stride);
            if (!slice.data || slice.geometry.stride < std::numeric_limits<int>::min() ||
                slice.geometry.stride > std::numeric_limits<int>::max() || slice.geometry.row_bytes < expected_row_bytes[plane] || static_cast<size_t>(slice.geometry.rows) < expected_rows ||
                stride_magnitude < expected_row_bytes[plane]) {
                last_error_ = "Invalid video plane view";
                av_frame_unref(av_frame);
                return false;
            }
            source_data[plane] = slice.data;
            source_linesize[plane] = slice.geometry.stride;
        }

        av_image_copy(av_frame->data,
                      av_frame->linesize,
                      source_data,
                      source_linesize,
                      format,
                      meta->width,
                      meta->height);
        SetAVFrameTime(input, av_frame);
        return true;
    }

    if (input.type == MediaType::AUDIO) {
        const auto* meta = input.AudioMeta();
        if (!meta) {
            last_error_ = "Audio MediaFrame has no metadata";
            return false;
        }

        const AVSampleFormat format = ToAVSampleFormat(meta->sample_format);
        if (format == AV_SAMPLE_FMT_NONE || meta->sample_rate <= 0 ||
            meta->channels <= 0 || meta->nb_samples <= 0) {
            last_error_ = "Invalid audio MediaFrame metadata";
            return false;
        }

        const uint64_t channel_layout = meta->channel_layout != 0
            ? meta->channel_layout
            : defaultChannelLayoutMask(meta->channels);
        if (channel_layout == 0) {
            last_error_ = "Audio MediaFrame has no supported channel layout";
            return false;
        }

        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? meta->channels : 1;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        if (bytes_per_sample <= 0 || plane_count <= 0 ||
            input.view.PlaneCount() != static_cast<std::size_t>(plane_count)) {
            last_error_ = "Invalid audio plane metadata";
            return false;
        }

        av_frame_unref(av_frame);
        av_frame->format = format;
        av_frame->sample_rate = meta->sample_rate;
        av_frame->nb_samples = meta->nb_samples;
        int ret = av_channel_layout_from_mask(&av_frame->ch_layout,
                                              channel_layout);
        if (ret < 0) {
            last_error_ = "Failed to set audio channel layout: " +
                          AvErrorString(ret);
            return false;
        }
        ret = av_frame_get_buffer(av_frame, 32);
        if (ret < 0) {
            last_error_ = "av_frame_get_buffer for audio failed: " +
                          AvErrorString(ret);
            return false;
        }

        const size_t one_plane_size = static_cast<size_t>(meta->nb_samples) *
                                      static_cast<size_t>(bytes_per_sample);
        if (one_plane_size > std::numeric_limits<size_t>::max() /
                static_cast<size_t>(meta->channels)) {
            last_error_ = "Audio plane size overflow";
            av_frame_unref(av_frame);
            return false;
        }
        const size_t packed_size = one_plane_size *
                                   static_cast<size_t>(meta->channels);
        for (int plane = 0; plane < plane_count; ++plane) {
            const size_t required_size = planar ? one_plane_size : packed_size;
            const auto* source = input.view.Plane(static_cast<std::size_t>(plane));
            if (!source || !source->IsValid() || source->geometry.rows != 1 ||
                source->geometry.row_bytes < required_size) {
                last_error_ = "Invalid audio plane range";
                av_frame_unref(av_frame);
                return false;
            }
            uint8_t* destination = av_frame->extended_data[plane];
            if (!destination) {
                last_error_ = "Audio AVFrame plane is null";
                av_frame_unref(av_frame);
                return false;
            }
            std::memcpy(destination, source->data, required_size);
        }

        SetAVFrameTime(input, av_frame);
        return true;
    }

    last_error_ = "Unsupported MediaFrame type";
    return false;
}

bool MediaFrameConverter::AvFrameToMediaFrame(const AVFrame& av_frame, MediaFrame* media_frame) {
    if (!media_frame) {
        last_error_ = "Output MediaFrame is null";
        return false;
    }
    // 后面是将avframe中的数据复制到 owned storage 中，所有权为 MediaFrame
    *media_frame = MediaFrame{};
    media_frame->backend = {};
    SetMediaFrameTime(av_frame, media_frame);

    if (av_frame.width > 0 && av_frame.height > 0) {
        const auto format = static_cast<AVPixelFormat>(av_frame.format);
        const PixelFormat pixel_format = FromAVPixelFormat(format);
        if (pixel_format == PixelFormat::kUnknown || !av_frame.data[0]) {
            last_error_ = "Unsupported or empty video AVFrame";
            return false;
        }
        // 计算视频缓冲区大小
        const int total_size = av_image_get_buffer_size(format, av_frame.width,
            av_frame.height, 1);
        if (total_size <= 0) {
            last_error_ = "Failed to calculate video buffer size: " + AvErrorString(total_size);
            return false;
        }
        // 分配视频缓冲区
        std::vector<uint8_t> packed(static_cast<size_t>(total_size));
        const uint8_t* source_data[4] = {
            av_frame.data[0],
            av_frame.data[1],
            av_frame.data[2],
            av_frame.data[3],
        };
        int ret = av_image_copy_to_buffer(
            packed.data(),
            total_size,
            source_data,
            av_frame.linesize,
            format,
            av_frame.width,
            av_frame.height,
            1);
        if (ret < 0) {
            last_error_ = "av_image_copy_to_buffer failed: " +
                          AvErrorString(ret);
            return false;
        }

        const int plane_count = av_pix_fmt_count_planes(format);
        int packed_linesize[4]{};
        ptrdiff_t packed_linesize_ptrdiff[4]{};
        size_t plane_sizes[4]{};
        if (plane_count <= 0 || plane_count > 4 ||
            av_image_fill_linesizes(packed_linesize, format, av_frame.width) < 0) {
            last_error_ = "Failed to calculate video plane layout";
            return false;
        }
        for (int plane = 0; plane < 4; ++plane) {
            packed_linesize_ptrdiff[plane] = packed_linesize[plane];
        }
        ret = av_image_fill_plane_sizes(plane_sizes, format, av_frame.height, packed_linesize_ptrdiff);
        if (ret < 0) {
            last_error_ = "Failed to calculate video plane sizes: " + AvErrorString(ret);
            return false;
        }

        VideoFrameMeta meta{};
        meta.pixel_format = pixel_format;
        meta.width = av_frame.width;
        meta.height = av_frame.height;

        media_frame->type = MediaType::VIDEO;
        media_frame->meta = meta;
        auto owner = std::make_shared<std::vector<uint8_t>>(std::move(packed));
        std::vector<MediaPlaneView> views;
        views.reserve(static_cast<std::size_t>(plane_count));
        size_t offset = 0;
        for (int p = 0; p < plane_count; ++p) {
            views.emplace_back(owner->data() + offset, plane_sizes[p],
                PlaneGeometry{packed_linesize[p], static_cast<size_t>(packed_linesize[p]),
                    plane_sizes[p] / static_cast<size_t>(packed_linesize[p])});
            offset += plane_sizes[p];
        }
        media_frame->view = MediaBufferView::FromContiguous(std::move(views),
            ConstBufferRegion{owner->data(), static_cast<size_t>(total_size)},
            owner);
        last_error_.clear();
        return true;
    }

    if (av_frame.nb_samples > 0 && av_frame.ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(av_frame.format);
        const SampleFormat sample_format = FromAVSampleFormat(format);
        const int channels = av_frame.ch_layout.nb_channels;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        if (sample_format == SampleFormat::Unknown ||
            av_frame.sample_rate <= 0 || bytes_per_sample <= 0) {
            last_error_ = "Unsupported or empty audio AVFrame";
            return false;
        }

        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? channels : 1;
        const size_t one_plane_size = static_cast<size_t>(av_frame.nb_samples) *
                                      static_cast<size_t>(bytes_per_sample);
        if (channels <= 0 || one_plane_size >
                std::numeric_limits<size_t>::max() / static_cast<size_t>(channels)) {
            last_error_ = "Invalid audio buffer size";
            return false;
        }
        const size_t packed_size = one_plane_size * static_cast<size_t>(channels);
        // planar：每个声道一个平面，每个平面包含 nb_samples 个样本。
        // packed：所有声道交错存放在同一个平面，所以同一个平面要包含
        // nb_samples * channels 个样本。两种布局的总字节数实际都等于：
        // nb_samples * bytes_per_sample * channels。
        const size_t total_size = packed_size;
        if (total_size == 0 || plane_count <= 0) {
            last_error_ = "Invalid audio buffer size";
            return false;
        }

        std::vector<uint8_t> packed(total_size);
        for (int plane = 0; plane < plane_count; ++plane) {
            const uint8_t* source = framePlane(av_frame, plane);
            if (!source) {
                last_error_ = "Audio AVFrame plane is null";
                return false;
            }

            const size_t copy_size = planar
                ? one_plane_size
                : one_plane_size * static_cast<size_t>(channels);
            const size_t destination_offset = planar
                ? static_cast<size_t>(plane) * one_plane_size
                : 0;
            std::memcpy(packed.data() + destination_offset,
                        source,
                        copy_size);
        }

        AudioFrameMeta meta{};
        meta.sample_format = sample_format;
        meta.sample_rate = av_frame.sample_rate;
        meta.channels = channels;
        meta.channel_layout = av_frame.ch_layout.order == AV_CHANNEL_ORDER_NATIVE
            ? av_frame.ch_layout.u.mask
            : 0;
        meta.nb_samples = av_frame.nb_samples;
        meta.bytes_per_sample = bytes_per_sample;
        meta.planar = planar;

        media_frame->type = MediaType::AUDIO;
        media_frame->meta = meta;
        auto owner = std::make_shared<std::vector<uint8_t>>(std::move(packed));
        std::vector<MediaPlaneView> views;
        views.reserve(static_cast<std::size_t>(plane_count));
        size_t offset = 0;
        for (int p = 0; p < plane_count; ++p) {
            const auto size = planar ? one_plane_size : packed_size;
            views.emplace_back(owner->data() + offset, size,
                PlaneGeometry{static_cast<ptrdiff_t>(size), size, 1});
            offset += size;
        }
        media_frame->view = MediaBufferView::FromContiguous(std::move(views),
            ConstBufferRegion{owner->data(), total_size}, owner);
        last_error_.clear();
        return true;
    }

    last_error_ = "AVFrame is neither a valid video nor audio frame";
    return false;
}

void MediaFrameConverter::SetAVFrameTime(const MediaFrame& input, AVFrame* output) {
    // MediaFrame 的时间单位是微秒。转换器内部的 AVFrame 也暂时约定使用
    // 微秒刻度；真正送入编码器时，编码器再按自己的 time_base 换算。
    // kNoTimestamp 是工程层的“无时间戳”标记，不能直接写入 AVFrame，
    // 必须转换为 FFmpeg 约定的 AV_NOPTS_VALUE。
    output->pts = IsValidTimestamp(input.time.pts_us) ? input.time.pts_us : AV_NOPTS_VALUE;
    output->pkt_dts = IsValidTimestamp(input.time.dts_us) ? input.time.dts_us : AV_NOPTS_VALUE;
    output->duration = IsValidTimestamp(input.time.duration_us) ? input.time.duration_us : 0;
}


void MediaFrameConverter::SetMediaFrameTime(const AVFrame& input, MediaFrame* output) {
    // Converter 当前约定 AVFrame 中的时间戳也使用微秒刻度，因此这里不做
    // time_base 换算。注意：kNoTimestamp 与 0 不同，0 是合法的首帧时间戳。
    output->time.pts_us = input.pts == AV_NOPTS_VALUE ? kNoTimestamp : input.pts;
    output->time.dts_us = input.pkt_dts == AV_NOPTS_VALUE ? kNoTimestamp : input.pkt_dts;
    output->time.duration_us = input.duration == AV_NOPTS_VALUE ? kNoTimestamp : input.duration;
}

bool MediaFrameConverter::AdoptAVFrame(AVFrame* av_frame, std::shared_ptr<MediaFrame>& output) {
    output.reset();
    if (!av_frame) {
        last_error_ = "AVFrame is null";
        return false;
    }

    auto owner = Media::OwnAVFrame(av_frame);
    MediaType media_type = MediaType::UNKNOWN;
    FrameMeta meta;
    if (!FillRawFrameMeta(*owner, media_type, meta, last_error_)) return false;

    MediaFrame result;
    result.type = media_type;
    result.meta = std::move(meta);
    result.view = MakeFFmpegFrameView(*owner, owner);
    if (result.view.PlaneCount() == 0) {
        last_error_ = "Invalid raw AVFrame layout";
        return false;
    }
    result.backend.type = BackendHandle::FFMPEG;
    result.backend.ptr = owner.get();
    SetMediaFrameTime(*owner, &result);
    output = std::make_shared<MediaFrame>(std::move(result));
    return true;
}


void MediaFrameConverter::Close() {
    if (ffmpeg_video_converter_) {
        ffmpeg_video_converter_->Close();
        ffmpeg_video_converter_.reset();
    }
    if (ffmpeg_audio_converter_) {
        ffmpeg_audio_converter_->Close();
        ffmpeg_audio_converter_.reset();
    }
    opened_ = false;
    video_config_ = {};
    audio_config_ = {};
    backend_ = ConvertBackend::FFmpeg;
    last_error_.clear();
#if CONVERT_STATS_ENABLE
    stats = ConvertStats{};
#endif
}

const std::string& MediaFrameConverter::LastError() {
    return last_error_;
}

#if CONVERT_STATS_ENABLE
void MediaFrameConverter::PrintStats() {
    uint64_t avg_convert_time = stats.convert_frames > 0 
        ? stats.total_convert_time_us / stats.convert_frames : 0;
        
    LOG_INFO("FFmpegConverterStats: convert_calls: {}, convert_frames: {}, convert_errors: {}", 
             stats.convert_calls, stats.convert_frames, stats.convert_errors);
    LOG_INFO("FFmpegConverterStats: total_convert_time(s): {}", stats.total_convert_time_us / 1000000.0);
    LOG_INFO("FFmpegConverterStats: max_convert_time(ms): {}", stats.max_convert_time_us / 1000.0);
    LOG_INFO("FFmpegConverterStats: min_convert_time(ms): {}", stats.min_convert_time_us / 1000.0);
    LOG_INFO("FFmpegConverterStats: avg_convert_time(ms): {}", avg_convert_time / 1000.0);
}
#endif
