#include "media/decoder/ffmpeg_decoder.h"
#include "media/ffmpeg_frame_owner.h"
#include "media/ffmpeg_packet_utils.h"
#include "media/converter/media_frame_converter.h"
#include "media/ffmpeg_format.h"
#include "media/ffmpeg_frame_view.h"
#include "common/log/logger.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <chrono>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}
using namespace Media;
namespace {

AVRational ToAVRational(const Rational& rational) {
    return AVRational{
        rational.num > 0 ? rational.num : 1,
        rational.den > 0 ? rational.den : AV_TIME_BASE,
    };
}

bool IsValidTimeBase(AVRational time_base) {
    return time_base.num > 0 && time_base.den > 0;
}

AVRational FrameTimeBase(const AVFrame* frame, const MediaStreamInfo& stream_info) {
    if (frame && IsValidTimeBase(frame->time_base)) {
        return frame->time_base;
    }
    return ToAVRational(stream_info.time_base);
}

int64_t TimestampToUs(int64_t timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE) {
        return kNoTimestamp;
    }

    constexpr AVRational kMicrosecondTimeBase{1, AV_TIME_BASE};
    return av_rescale_q(timestamp, time_base, kMicrosecondTimeBase);
}

int64_t FramePts(const AVFrame* frame) {
    if (!frame) {
        return AV_NOPTS_VALUE;
    }
    return frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
}

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace



FFmpegDecoder::FFmpegDecoder() {
#if DECODE_STATS_ENABLE
    stats = DecoderStats{};
#endif
}



FFmpegDecoder::~FFmpegDecoder() {
    Close();
#if DECODE_STATS_ENABLE
    ResetStats();
#endif
}

bool FFmpegDecoder::Open(const MediaStreamInfo& info) {
    // Open 可能被重连/重建路径重复调用。先关闭旧上下文，保证 extradata、
    // codec 状态和旧回调不会泄漏到新一代媒体流。
    Close();

    if (info.codec_type == CodecType::UNKNOWN) {
        LOG_ERROR("FFmpegDecoder:Open rejected: unknown codec");
        return false;
    }

    // 1. CodecType -> AVCodecID
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (info.codec_type) {
        case CodecType::H264:  codec_id = AV_CODEC_ID_H264; break;
        case CodecType::H265:  codec_id = AV_CODEC_ID_HEVC; break;
        case CodecType::AAC:   codec_id = AV_CODEC_ID_AAC;  break;
        case CodecType::OPUS:  codec_id = AV_CODEC_ID_OPUS; break;
        case CodecType::G711A: codec_id = AV_CODEC_ID_PCM_ALAW; break;
        case CodecType::G711U: codec_id = AV_CODEC_ID_PCM_MULAW; break;
        case CodecType::JPEG:  codec_id = AV_CODEC_ID_MJPEG; break;
        default:
            LOG_ERROR("FFmpegDecoder:Open: unsupported codec type {}", static_cast<int>(info.codec_type));
            return false;
    }

    // 2. 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(codec_id);
    if (!decoder) {
        LOG_ERROR("FFmpegDecoder:Open: avcodec_find_decoder failed for id={}", static_cast<int>(codec_id));
        return false;
    }

    // 3. 分配解码器上下文
    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        LOG_ERROR("FFmpegDecoder:Open: avcodec_alloc_context3 failed");
        return false;
    }

    // 4. 设置解码参数
    if (info.media_type == MediaType::VIDEO) {
        codec_ctx_->width       = info.get_detail<VideoStreamInfo>().width;
        codec_ctx_->height      = info.get_detail<VideoStreamInfo>().height;
        codec_ctx_->pix_fmt     = AV_PIX_FMT_NONE;   // 由解码器自动检测
        codec_ctx_->thread_count = 1;                  // 单线程，保证确定性
    } else if (info.media_type == MediaType::AUDIO) {
        const auto& audio_info = info.get_detail<AudioStreamInfo>();
        codec_ctx_->sample_rate    = audio_info.sample_rate;
        codec_ctx_->ch_layout.nb_channels = audio_info.channels;
        codec_ctx_->ch_layout.u.mask = audio_info.channel_layout;
        codec_ctx_->sample_fmt   = AV_SAMPLE_FMT_NONE; // 由解码器自动检测
    }

    // 5. 设置 extradata（SPS/PPS 等）
    codec_ctx_->pkt_timebase = ToAVRational(info.time_base);

    if (!info.extra_data.empty()) {
        codec_ctx_->extradata_size = static_cast<int>(info.extra_data.size());
        codec_ctx_->extradata = static_cast<uint8_t*>(
            av_malloc(info.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_ctx_->extradata) {
            LOG_ERROR("FFmpegDecoder:Open: av_malloc for extradata failed");
            avcodec_free_context(&codec_ctx_);
            return false;
        }
        std::memcpy(codec_ctx_->extradata, info.extra_data.data(),
                    info.extra_data.size());
        std::memset(codec_ctx_->extradata + info.extra_data.size(), 0,
                    AV_INPUT_BUFFER_PADDING_SIZE);
    }

    // 6. 打开解码器
    int ret = avcodec_open2(codec_ctx_, decoder, nullptr);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("FFmpegDecoder:Open: avcodec_open2 failed: {}", buf);
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    stream_info_ = info;
    if (info.media_type == MediaType::VIDEO) {
        info.Dump();
    } else if (info.media_type == MediaType::AUDIO) {
        info.Dump();
    }
    return true;
}

bool FFmpegDecoder::Flush() {
    if (!codec_ctx_) {
        return true;
    }

    // FFmpeg 的 send/receive API 要求通过 nullptr 明确进入 drain 状态。
    // 这里只负责输出残留帧，不释放上下文，便于调用方在需要时继续查询状态。
    const int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("FFmpegDecoder:Flush: avcodec_send_packet failed: {}", buf);
        return false;
    }
    return receiveFrames();
}

void FFmpegDecoder::Close() {
    if (codec_ctx_) {
        // Close 只释放资源，不再隐式触发回调。需要保留尾帧时必须先显式 Flush。
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    stream_info_ = {};
}

bool FFmpegDecoder::Decode(std::shared_ptr<MediaPacket> packet) {
    if (!codec_ctx_) {
        LOG_ERROR("FFmpegDecoder:Decode: codec_ctx_ is null");
        return false;
    }
    if (!packet || !packet->buffer) {
#if DECODE_STATS_ENABLE    
        ++stats.decode_errors;
#endif
        LOG_ERROR("FFmpegDecoder:Decode: invalid packet");
        return false;
    }
#if DECODE_STATS_ENABLE    
    ++stats.decode_calls;
#endif
    // FFmpegPuller 输出的 packet 已经持有 AVPacket，可以直接复用。
    // AVTP/RTP 等输入通常只持有普通 MediaPacketBuffer，这里临时包装成 AVPacket。
    const AVPacket* avpkt = nullptr;
    AVPacket* temporary_packet = nullptr;
    if (packet->backend.type == BackendHandle::FFMPEG) {
        avpkt = Media::GetBackendPacket(*packet);
        if (!avpkt) {
#if DECODE_STATS_ENABLE    
            ++stats.decode_errors;
#endif
            LOG_ERROR("FFmpegDecoder:Decode: backend AVPacket is null");
            return false;
        }

        // AVPacket 自身不携带 time_base，MediaPacket 才是跨节点的时间基契约。
        // 来源和 decoder time base 不一致时先做引用拷贝再重标定，避免修改
        // 上游共享 packet，也避免 FFmpeg 把错误 tick 当成当前 codec 的时间轴。
        const AVRational source_time_base = ToAVRational(packet->time_base);
        const AVRational decoder_time_base = IsValidTimeBase(codec_ctx_->pkt_timebase)
            ? codec_ctx_->pkt_timebase
            : source_time_base;
        if (source_time_base.num != decoder_time_base.num ||
            source_time_base.den != decoder_time_base.den) {
            temporary_packet = av_packet_alloc();
            if (!temporary_packet || av_packet_ref(temporary_packet, avpkt) < 0) {
                av_packet_free(&temporary_packet);
#if DECODE_STATS_ENABLE    
                ++stats.decode_errors;
#endif
                LOG_ERROR("FFmpegDecoder:Decode: AVPacket timestamp copy failed");
                return false;
            }
            av_packet_rescale_ts(temporary_packet,
                                 source_time_base,
                                 decoder_time_base);
            avpkt = temporary_packet;
        }
    } else {
        const std::size_t packet_size = packet->buffer->Size();
        if (!packet->buffer->Data() || packet_size == 0 ||
            packet_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
#if DECODE_STATS_ENABLE    
            ++stats.decode_errors;
#endif
            LOG_ERROR("FFmpegDecoder:Decode: invalid raw packet");
            return false;
        }

        temporary_packet = av_packet_alloc();
        if (!temporary_packet ||
            av_new_packet(temporary_packet, static_cast<int>(packet_size)) < 0) {
            av_packet_free(&temporary_packet);
#if DECODE_STATS_ENABLE    
            ++stats.decode_errors;
#endif
            LOG_ERROR("FFmpegDecoder:Decode: AVPacket allocation failed");
            return false;
        }

        std::memcpy(temporary_packet->data, packet->buffer->Data(), packet_size);

        // MediaPacket 的时间戳单位由 packet->time_base 描述；而 libavcodec 期望
        // AVPacket 时间戳落在 codec_ctx_->pkt_timebase 下。这里做一次显式换算，
        // 使 AVTP 这类微秒时间戳，以及测试中从 FFmpegPuller 复制出来的微秒
        // MediaPacket，都能进入同一条 decoder 路径。
        const AVRational source_time_base = ToAVRational(packet->time_base);
        const AVRational decoder_time_base = IsValidTimeBase(codec_ctx_->pkt_timebase)
            ? codec_ctx_->pkt_timebase
            : source_time_base;
        const auto rescale_timestamp = [&](int64_t timestamp) -> int64_t {
            return timestamp == AV_NOPTS_VALUE
                ? AV_NOPTS_VALUE
                : av_rescale_q(timestamp, source_time_base, decoder_time_base);
        };

        temporary_packet->pts = rescale_timestamp(packet->pts);
        temporary_packet->dts = rescale_timestamp(packet->dts);
        temporary_packet->duration = rescale_timestamp(packet->duration);
        if (packet->keyframe) {
            temporary_packet->flags |= AV_PKT_FLAG_KEY;
        }
        avpkt = temporary_packet;
    }
    
#if DECODE_STATS_ENABLE
    int64_t decode_start_time = Now();
#endif
    
    const int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&temporary_packet);
    if (ret < 0) {
#if DECODE_STATS_ENABLE    
        ++stats.decode_errors;
#endif        
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("avcodec_send_packet failed: {}", buf);
        return false;
    }
#if DECODE_STATS_ENABLE    
    ++stats.decode_packets;
#endif    

    bool receive_result = receiveFrames();
    
#if DECODE_STATS_ENABLE
    int64_t decode_time_us = Now() - decode_start_time;
    stats.total_decode_time_us += decode_time_us;
    if (decode_time_us > stats.max_decode_time_us) {
        stats.max_decode_time_us = decode_time_us;
    }
    if (decode_time_us < stats.min_decode_time_us) {
        stats.min_decode_time_us = decode_time_us;
    }
#endif
    
    return receive_result;
}

void FFmpegDecoder::SetFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    frame_cb_ = std::move(cb);
}

bool FFmpegDecoder::receiveFrames() {
    if (!codec_ctx_) return false;
    for (;;) {
        std::unique_ptr<AVFrame, Media::AVFrameDeleter> decoded(av_frame_alloc());
        if (!decoded) {
            LOG_ERROR("av_frame_alloc failed");
            return false;
        }
        const int ret = avcodec_receive_frame(codec_ctx_, decoded.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            char error[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(error, sizeof(error), ret);
            LOG_ERROR("avcodec_receive_frame failed: {}", error);
            return false;
        }
        const AVRational time_base = FrameTimeBase(decoded.get(), stream_info_);
        const MediaTime time{
            TimestampToUs(FramePts(decoded.get()), time_base),
            TimestampToUs(decoded->pkt_dts, time_base),
            TimestampToUs(decoded->duration, time_base)};
        std::shared_ptr<MediaFrame> frame;
#if RAW_FRAME_BUFFER
        if (!MediaFrameConverter::AdoptAVFrame(decoded.release(), frame)) {
#else
        frame = std::make_shared<MediaFrame>();
        if (!MediaFrameConverter::AvFrameToMediaFrame(*decoded, frame.get())) {
#endif
            LOG_ERROR("Decoded frame conversion failed: {}", MediaFrameConverter::LastError());
            return false;
        }
        frame->time = time;
#if DECODE_STATS_ENABLE
        ++stats.decode_frames;
#endif
        FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            callback = frame_cb_;
        }
        if (callback) callback(std::move(frame));
    }
}
#if DECODE_STATS_ENABLE    
void FFmpegDecoder::PrintStats() const {
    uint64_t avg_decode_time = stats.decode_frames > 0 
        ? stats.total_decode_time_us / stats.decode_frames 
        : 0;
        
    LOG_INFO("FFmpegDecoderStats: decode_calls: {}, decode_packets: {}, decode_frames: {}, decode_errors: {}", 
             stats.decode_calls, stats.decode_packets, stats.decode_frames, stats.decode_errors);
    LOG_INFO("FFmpegDecoderStats: total_decode_time(s): {}", stats.total_decode_time_us / 1000000.0);
    LOG_INFO("FFmpegDecoderStats: max_decode_time(ms): {}", stats.max_decode_time_us / 1000.0);
    LOG_INFO("FFmpegDecoderStats: min_decode_time(ms): {}", stats.min_decode_time_us / 1000.0);
    LOG_INFO("FFmpegDecoderStats: avg_decode_time(ms): {}", avg_decode_time / 1000.0);
}
#endif
