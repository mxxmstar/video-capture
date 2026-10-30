#include "media/encoder/generic/ffmpeg_encoder.h"
#include "media/converter/generic/media_frame_converter.h"
#include "media/generic/ffmpeg_format.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/mem.h>
#include <libavformat/avformat.h>
}

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

FramePtr MakeAudio(int count, int offset, int rate, SampleFormat format,
                   bool timestamps, int channels = 2) {
    AVFrame* source = av_frame_alloc();
    Require(source != nullptr, "allocate source");
    source->format = format == SampleFormat::S16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S16P;
    source->sample_rate = rate;
    source->nb_samples = count;
    av_channel_layout_default(&source->ch_layout, channels);
    Require(av_frame_get_buffer(source, 0) >= 0, "allocate samples");
    for (int i = 0; i < count; ++i) {
        for (int channel = 0; channel < channels; ++channel) {
            const int16_t value = static_cast<int16_t>(12000 *
                std::sin(2 * 3.141592653589793 * (440 + channel * 110) * (i + offset) / rate));
            const int plane = format == SampleFormat::S16 ? 0 : channel;
            const int index = format == SampleFormat::S16 ? i * channels + channel : i;
            std::memcpy(source->extended_data[plane] + index * sizeof(value), &value, sizeof(value));
        }
    }
    FramePtr frame;
    Require(MediaFrameConverter::AdoptAVFrame(source, frame), "adopt samples");
    frame->time.pts_us = timestamps ? 1000000 + av_rescale_q(offset, {1, rate}, {1, 1000000}) : kNoTimestamp;
    return frame;
}

void Run(CodecType codec, const char* name, int rate, SampleFormat format,
         bool timestamps, bool custom_time_base, int channels = 2) {
    EncoderConfig config;
    config.media_type = MediaType::AUDIO;
    config.codec_type = codec;
    config.specific = AudioEncoderConfig{};
    config.audio().sample_rate = rate;
    config.audio().channels = channels;
    config.audio().sample_format = format;
    config.encoder_name = name;
    config.bitrate = codec == CodecType::G726 ? 32000 : 128000;
    config.global_header = true;
    if (custom_time_base) {
        config.time_base_num = 1;
        config.time_base_den = 1000000;
    }
    FFmpegEncoder encoder;
    Require(encoder.Open(config), "open encoder");
    const auto info = encoder.GetOutputInfo();
    Require(info.media_type == MediaType::AUDIO && info.audio().sample_rate == rate &&
        info.audio().channels == channels, "audio output metadata");
    std::vector<PacketPtr> packets;
    Require(!encoder.Encode(MakeAudio(100, 0, rate + 1, format, timestamps, channels), packets),
        "reject mismatching input rate");
    auto malformed = std::make_shared<MediaFrame>();
    malformed->type = MediaType::AUDIO; // Wrong metadata variant must not throw.
    Require(!encoder.Encode(malformed, packets), "reject malformed metadata");
    const int chunks[] = {137, 200, 3000, 17, 1024, 501};
    int total = 0;
    for (int count : chunks) {
        Require(encoder.Encode(MakeAudio(count, total, rate, format, timestamps, channels), packets), "encode samples");
        total += count;
    }
    Require(encoder.Flush(packets), "flush audio tail");
    const auto packet_count = packets.size();
    Require(encoder.Flush(packets) && packets.size() == packet_count, "idempotent flush");
    Require(!encoder.Encode(MakeAudio(100, total, rate, format, timestamps, channels), packets), "reject input after flush");
    Require(!packets.empty(), "audio packets produced");
    const AVRational time_base{info.time_base.num, info.time_base.den};
    int64_t last_pts = kNoTimestamp;
    int64_t duration = 0;
    for (const auto& packet : packets) {
        Require(packet->type == MediaType::AUDIO && packet->codec == codec &&
            packet->time_base.num == time_base.num && packet->time_base.den == time_base.den,
            "packet metadata");
        Require(IsValidTimestamp(packet->pts) && packet->duration > 0 &&
            (!IsValidTimestamp(last_pts) || packet->pts > last_pts), "sample based packet timeline");
        last_pts = packet->pts;
        duration += packet->duration;
    }
    if (codec == CodecType::G711A) {
        Require(packets.front()->pts == (timestamps ? av_rescale_q(1000000, {1,1000000}, time_base) : 0), "first audio pts");
        Require(av_rescale_q(duration, time_base, {1, rate}) == total, "PCM duration has no lost samples");
    }

    const AVCodec* decoder = avcodec_find_decoder(Media::ToAVCodecID(codec));
    AVCodecContext* context = avcodec_alloc_context3(decoder);
    Require(context != nullptr, "allocate decoder");
    context->sample_rate = rate;
    context->pkt_timebase = time_base;
    context->bit_rate = config.bitrate;
    if (codec == CodecType::G726) context->bits_per_coded_sample = 4;
    av_channel_layout_default(&context->ch_layout, channels);
    if (!info.extra_data.empty()) {
        context->extradata_size = static_cast<int>(info.extra_data.size());
        context->extradata = static_cast<uint8_t*>(av_mallocz(info.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        Require(context->extradata != nullptr, "allocate decoder extradata");
        std::memcpy(context->extradata, info.extra_data.data(), info.extra_data.size());
    }
    Require(avcodec_open2(context, decoder, nullptr) >= 0, "open decoder");
    AVFrame* decoded = av_frame_alloc();
    int decoded_samples = 0;
    bool nonzero = false;
    auto receive = [&] {
        for (;;) {
            const int ret = avcodec_receive_frame(context, decoded);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            Require(ret >= 0, "decode packet");
            decoded_samples += decoded->nb_samples;
            const int bytes = decoded->nb_samples * av_get_bytes_per_sample(static_cast<AVSampleFormat>(decoded->format));
            for (int i = 0; i < bytes; ++i) nonzero = nonzero || decoded->extended_data[0][i] != 0;
            av_frame_unref(decoded);
        }
    };
    for (const auto& packet : packets) {
        const int sent = avcodec_send_packet(context, static_cast<AVPacket*>(packet->backend.ptr));
        if (sent < 0) std::cerr << "decoder send error=" << sent << " codec=" << name << '\n';
        Require(sent >= 0, "send decoder packet");
        receive();
    }
    Require(avcodec_send_packet(context, nullptr) >= 0, "flush decoder");
    receive();
    Require(nonzero, "decoded signal is not silence");
    Require(decoded_samples >= total && decoded_samples <= total + 2048, "decoded sample count includes tail");
    if (codec == CodecType::G711A) Require(decoded_samples == total, "PCM sample count exact");
    av_frame_free(&decoded);
    avcodec_free_context(&context);
    encoder.Close();
    Require(encoder.Open(config), "reopen encoder");
    std::vector<PacketPtr> empty;
    Require(encoder.Flush(empty) && empty.empty(), "empty flush after reopen");
    std::cout << name << " format=" << static_cast<int>(format) << " tb=" << time_base.num << "/" << time_base.den
              << " input=" << total << " decoded=" << decoded_samples << " packets=" << packet_count << '\n';
}

/// @brief 用真实 WAV 文件做端到端回归：WAV 解码 -> AAC 编码 -> 写入容器文件
/// @param input_path 输入 WAV 路径
/// @param output_path 输出 AAC 容器路径（由扩展名推导 muxer）
/// @note 与 Run() 的合成信号用例互补：这里验证真实文件格式、真实 packet 分块
///       以及 muxer 写入路径，任何一步失败都会抛异常终止。
void RunFile(const char* input_path, const char* output_path) {
    // 打开输入文件并探测流信息。find_stream_info 之后 codecpar 才有
    // 可靠的采样率/声道/采样格式，供后面构造解码器和编码器使用。
    AVFormatContext* input = nullptr;
    Require(avformat_open_input(&input, input_path, nullptr, nullptr) >= 0, "open input WAV");
    Require(avformat_find_stream_info(input, nullptr) >= 0, "probe input WAV");
    // 只处理第一条音频流，返回其在 streams 中的索引。
    const int index = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    Require(index >= 0, "find WAV audio");
    AVStream* stream = input->streams[index];
    // 按容器声明的 codec_id 查找解码器，并把流参数拷贝到解码上下文。
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    Require(decoder && avcodec_parameters_to_context(decoder, stream->codecpar) >= 0 &&
        avcodec_open2(decoder, codec, nullptr) >= 0, "open WAV decoder");
    FFmpegEncoder encoder;
    // 编码器和输出容器延迟到第一帧解码成功后创建：只有解码出的 AVFrame
    // 才携带真实采样率/声道/采样格式，用它可以避免依赖 WAV 头的假设。
    bool opened = false;
    AVFormatContext* output = nullptr;
    // 编码器输出的时间基。与输出流的时间基不同，写包前需要重标定。
    AVRational encoder_time_base{};
    // 累计送入编码器的输入样本数，用于按样本数（而非帧数）生成连续 pts。
    int64_t samples = 0;
    int packet_count = 0;
    // 把编码器产出的 MediaPacket 写入输出容器的统一出口。
    // 编码阶段每次 Encode/Flush 都可能吐出 0..N 个包，全部经由此处落地。
    auto write = [&](std::vector<PacketPtr>& packets) {
        for (const auto& packet : packets) {
            // MediaPacket 的 backend.ptr 只是非拥有指针，必须 clone 出一份
            // 独立 AVPacket，因为 muxer 会接管其生命周期。
            AVPacket* native = av_packet_clone(static_cast<AVPacket*>(packet->backend.ptr));
            Require(native != nullptr, "clone AAC packet");
            // 编码器 time_base 与输出流 time_base 通常不同（例如 1/48000 与
            // 1/1000），必须重标定，否则时长会被放大或缩小若干倍。
            av_packet_rescale_ts(native, encoder_time_base, output->streams[0]->time_base);
            // 输出容器只有一个音频流，索引固定为 0。
            native->stream_index = 0;
            // interleaved_write_frame 成功后接管 native 所有权，因此随后置空。
            const int ret = av_interleaved_write_frame(output, native);
            av_packet_free(&native);
            Require(ret >= 0, "write AAC output");
            ++packet_count;
        }
    };
    // 复用同一个 AVFrame 接收解码结果，避免每帧分配。
    AVFrame* decoded = av_frame_alloc();
    // 排空解码器：每次送入 packet 后调用，直到 EAGAIN/EOF。
    // 一帧输入 packet 可能产出多帧（或有延迟），所以必须是循环。
    auto receive = [&] {
        for (;;) {
            const int ret = avcodec_receive_frame(decoder, decoded);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            Require(ret >= 0, "decode WAV frame");
            // 首帧到达时完成编码器与输出容器的初始化（只执行一次）。
            if (!opened) {
                EncoderConfig cfg;
                cfg.media_type = MediaType::AUDIO;
                cfg.codec_type = CodecType::AAC;
                cfg.encoder_name = "aac";
                cfg.specific = AudioEncoderConfig{};
                // 输入形状完全取自真实解码帧，保证后续 encodeAudioFrame 的
                // 形状校验能通过，converter 因此只做采样格式转换。
                cfg.audio().sample_rate = decoded->sample_rate;
                cfg.audio().channels = decoded->ch_layout.nb_channels;
                cfg.audio().sample_format = Media::FromAVSampleFormat(static_cast<AVSampleFormat>(decoded->format));
                cfg.bitrate = 128000;
                // AAC 的 extradata(AudioSpecificConfig) 需要写进容器头。
                cfg.global_header = true;
                Require(encoder.Open(cfg), "open real WAV AAC encoder");
                // 编码器实际输出的时间基可能与配置推导值不同，必须回读。
                const auto info = encoder.GetOutputInfo();
                encoder_time_base = {info.time_base.num, info.time_base.den};
                // 输出格式由输出路径扩展名推导（.m4a/.aac 等），容器类型交给 FFmpeg。
                Require(avformat_alloc_output_context2(&output, nullptr, nullptr, output_path) >= 0 && output,
                    "allocate AAC container");
                AVStream* out_stream = avformat_new_stream(output, nullptr);
                Require(out_stream != nullptr, "create AAC stream");
                // 流的 time_base 与编码器保持一致，写包时只需做等价重标定。
                out_stream->time_base = encoder_time_base;
                auto* params = out_stream->codecpar;
                params->codec_type = AVMEDIA_TYPE_AUDIO;
                params->codec_id = AV_CODEC_ID_AAC;
                params->sample_rate = info.audio().sample_rate;
                av_channel_layout_default(&params->ch_layout, info.audio().channels);
                params->bit_rate = cfg.bitrate;
                // AAC 固定帧长 1024；initial_padding 用于标记编码器引入的
                // 前置延迟（解码端据此裁剪，避免开头出现 1024 个样本的空隙）。
                params->frame_size = 1024;
                params->initial_padding = 1024;
                // 拷贝编码器产生的 AudioSpecificConfig，解码端依赖它初始化。
                params->extradata_size = static_cast<int>(info.extra_data.size());
                params->extradata = static_cast<uint8_t*>(av_mallocz(info.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                Require(params->extradata != nullptr, "allocate AAC metadata");
                std::memcpy(params->extradata, info.extra_data.data(), info.extra_data.size());
                // 打开输出 IO 并写容器头（global_header 在此写入 extradata）。
                Require(avio_open(&output->pb, output_path, AVIO_FLAG_WRITE) >= 0 &&
                    avformat_write_header(output, nullptr) >= 0, "open AAC output");
                opened = true;
            }
            FramePtr frame;
            // clone 是因为下方 unref(decoded) 会复用该 AVFrame，而 MediaFrame
            // 需要独立持有数据；AdoptAVFrame 接管 clone 的所有权。
            Require(MediaFrameConverter::AdoptAVFrame(av_frame_clone(decoded), frame), "adopt WAV frame");
            // 按累计样本数换算微秒 pts（而非帧序号），避免非整数帧长累积漂移。
            frame->time.pts_us = av_rescale_q(samples, {1, decoded->sample_rate}, {1,1000000});
            samples += decoded->nb_samples;
            // 每次 Encode 只产出 0..N 个包：AAC 需要攒满 1024 样本才吐包，
            // 所以多数调用返回空列表，flush 时才吐出尾部。
            std::vector<PacketPtr> packets;
            Require(encoder.Encode(frame, packets), "encode WAV frame");
            write(packets);
            av_frame_unref(decoded);
        }
    };
    // 逐包读取并送入解码器；非音频流的包（如容器内的其他流）直接丢弃。
    AVPacket* packet = av_packet_alloc();
    int read = 0;
    while ((read = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == index) {
            Require(avcodec_send_packet(decoder, packet) >= 0, "send WAV packet");
            receive();
        }
        av_packet_unref(packet);
    }
    Require(read == AVERROR_EOF, "read WAV to EOF");
    // 送入 nullptr 冲刷解码器内部延迟，再排空一次，确保最后几帧不丢。
    Require(avcodec_send_packet(decoder, nullptr) >= 0, "flush WAV decoder");
    receive();
    // 若一帧都没解码出来，说明输入为空或解码失败，后续 output 也未初始化。
    Require(opened, "WAV contains samples");
    // 冲刷编码器：把 FIFO 残料补静音成整帧吐出，并触发编码器 EOF。
    // 这一步的包必须写入，否则音频尾部会缺失。
    std::vector<PacketPtr> packets;
    Require(encoder.Flush(packets), "flush real WAV AAC encoder");
    write(packets);
    // 回写容器尾部索引（mp4 的 moov、aac 的结束标记等），必须在关闭 IO 前完成。
    Require(av_write_trailer(output) >= 0, "finalize AAC container");
    // 逆序释放：IO -> 容器 -> 复用资源 -> 解码器 -> 输入容器。
    avio_closep(&output->pb);
    avformat_free_context(output);
    av_packet_free(&packet);
    av_frame_free(&decoded);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    // 打印统计：samples 是输入侧累计样本数，packet_count 是实际写入容器的
    // AAC 包数。两者不是简单比例关系（编码器有初始延迟和尾部补静音）。
    std::cout << "Real WAV: samples=" << samples << " AAC packets=" << packet_count
              << " output=" << output_path << '\n';
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            RunFile(argv[1], argv[2]);
            return 0;
        }
        Run(CodecType::AAC, "aac", 48000, SampleFormat::S16, true, false);
        Run(CodecType::AAC, "aac", 44100, SampleFormat::S16P, false, true);
        Run(CodecType::G711A, "pcm_alaw", 8000, SampleFormat::S16P, true, true);
        Run(CodecType::G711A, "pcm_alaw", 8000, SampleFormat::S16, false, false);
        Run(CodecType::OPUS, "libopus", 48000, SampleFormat::S16, true, false);
        Run(CodecType::G726, "g726", 8000, SampleFormat::S16, true, false, 1);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}