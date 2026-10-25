#pragma once
/// @file media_frame.hpp
/// 视频帧（Frame）定义，包含解码后的图像数据及其元信息。

#include <cstdint>
#include <cstddef>
#include <variant>
#include <memory>
#include "media_buffer.h"
#include "media_packet.h"

/// 像素格式
enum class PixelFormat {
    kUnknown = 0,
    kNV12,     ///< YUV420sp (Y + UV 交错)
    kNV21,     ///< YUV420sp (UV 顺序相反)
    kI420,     ///< YUV420p (三个平面)
    kBGR24,    ///< BGR 24 位
    kRGB24,    ///< RGB 24 位
    kGRAY8,    ///< 8 位灰度
    kYUYV422,  ///< Packed Y0 U0 Y1 V0, 4:2:2
};

/// @brief 音频采样格式
enum class SampleFormat {
    Unknown = 0,
    U8,        ///< 8 位无符号整数
    S16,       ///< 16 位有符号整数
    S32,       ///< 32 位有符号整数
    FLT,       ///< 单精度浮点数
    DBL,       ///< 双精度浮点数
    U8P,       ///< 8 位无符号整数，平面格式
    S16P,      ///< 16 位有符号整数，平面格式
    S32P,      ///< 32 位有符号整数，平面格式
    FLTP,      ///< 单精度浮点数，平面格式
    DBLP,      ///< 双精度浮点数，平面格式
};

/// @brief 时间戳
struct MediaTime {
    int64_t pts_us{0};   ///< 显示时间戳（微秒）
    int64_t dts_us{0};   ///< 解码时间戳（微秒）
    int64_t duration_us{0}; ///< 持续时间（微秒）
};

/// @brief 平面信息
struct PlaneInfo {
    int32_t offset{0}; ///< 平面数据偏移（字节）
    int32_t stride{0}; ///< 平面行跨度（字节）
    int32_t size{0};     ///< 平面数据大小（字节）
};

/// @brief 带 stride 填充的源缓冲区中单个平面的布局参数
struct StridedPlaneEntry {
    int rows = 0;               ///< 本平面行数
    int row_bytes = 0;          ///< 每行有效字节数（不含填充）
    int source_stride = 0;      ///< 缓冲区行跨度（含填充）
    std::size_t source_offset = 0; ///< 本平面在源缓冲区中的字节偏移
};

/// @brief 带 stride 填充的源缓冲区的完整平面布局
struct SourcePlaneLayout {
    int plane_count = 0;
    StridedPlaneEntry planes[3];

    bool is_empty() const { return plane_count <= 0; }

    /// 根据像素格式、宽高和源行跨度计算各平面的布局参数
    static SourcePlaneLayout Compute(PixelFormat format, int width, int height, int source_stride);
};

/// @brief struct VideoFrameMeta 视频帧元数据
struct VideoFrameMeta {
    PixelFormat pixel_format{PixelFormat::kUnknown}; ///< 像素格式
    int32_t     width{0};                           ///< 图像宽度（像素）
    int32_t     height{0};                          ///< 图像高度（像素）
    int32_t     plane_count{0};                        ///< 平面数量
    PlaneInfo   plane_info[8] {0}; ///< 平面信息
};

struct AudioFrameMeta {
    SampleFormat sample_format{SampleFormat::Unknown};  ///< 采样格式
    int sample_rate{0};                              ///< 采样率（Hz）
    int channels{0};                                 ///< 通道数
    uint64_t channel_layout{0};                      ///< 通道布局
    int nb_samples{0};                               ///< 样本数
    int bytes_per_sample{0};                        ///< 每个样本字节数
    bool planar{false};                             ///< 是否为平面格式
    int32_t plane_count{0};                         ///< 平面数量
    PlaneInfo planes[8]{};                          ///< 平面信息
};

using FrameMeta = std::variant<VideoFrameMeta, AudioFrameMeta>;

/// @brief 紧凑帧中单个平面的视图
struct PlaneSlice {
    const uint8_t* data = nullptr; ///< 平面起始地址
    int stride = 0;               ///< 行跨度（字节）
    int row_bytes = 0;            ///< 每行有效像素字节数
    int rows = 0;                 ///< 本平面行数
};

/// 媒体帧：解码后的图像数据及其描述信息
class MediaFrame {
public:
    MediaType    type{MediaType::VIDEO};              ///< 媒体类型（VIDEO/AUDIO）
    MediaTime   time;                               ///< 时间戳信息
    FrameMeta meta;                                 ///< 媒体帧元数据

    // 普通帧通常由 SimpleBuffer 持有连续数据；FFmpeg 后端帧也可以由
    // FFmpegRawFrameBuffer 持有多平面 AVFrame，此时不能通过 Data()/Size()
    // 访问整帧，而应使用 backend.ptr 或 raw buffer 的平面接口。    
    std::shared_ptr<IMediaBuffer> buffer;
    MediaBufferView view;           ///< 媒体数据缓冲区    
    BackendHandle backend;                          ///< 所有权句柄

    /// @brief 将带 stride 填充的源数据紧凑打包为自描述的 MediaFrame（零拷贝版本）
    /// 源数据由 shared_ptr 持有所有权。当布局已满足紧凑条件
    /// （各 plane stride==row_bytes 且平面间连续）时，直接包装为 SimpleBufferView 跳过 memcpy；
    /// 否则从 shared_ptr 读取并拷贝到 SimpleBuffer。
    /// @param source_data  拥有源数据所有权的 shared_ptr
    /// @param source_bytes 源缓冲区总大小
    /// @param format       像素格式
    /// @param width        帧宽度（像素）
    /// @param height       帧高度（像素）
    /// @param source_stride 源数据行跨度（含对齐填充）
    static std::shared_ptr<MediaFrame> CreateCompact(std::shared_ptr<const uint8_t> source_data, std::size_t source_bytes,
        PixelFormat format, int width, int height, int source_stride);

    /// @brief 纯拷贝版本：原始指针 + 不做所有权假设，始终 memcpy
    static std::shared_ptr<MediaFrame> CreateCompact(const unsigned char* source_data, std::size_t source_bytes,
        PixelFormat format, int width, int height, int source_stride);

    /// @brief 拷贝到外部预分配缓冲区，返回的 MediaFrame 通过 SimpleBufferView 引用目标内存
    ///       调用方需保证 dest 在帧存活期间有效
    /// @param dest          预分配的目标缓冲区
    /// @param dest_capacity 目标缓冲区容量，必须 >= 紧凑格式所需大小
    /// @return nullptr 表示参数非法或容量不足
    static std::shared_ptr<MediaFrame> CreateCompact(const unsigned char* source_data, std::size_t source_bytes,
        PixelFormat format, int width, int height, int source_stride,
        uint8_t* dest, std::size_t dest_capacity);

    /// @brief 从紧凑 MediaFrame 中解析指定平面的视图
    /// @return PlaneSlice，若 index 越界或元数据无效则 data 为 nullptr
    PlaneSlice GetPlaneSlice(int plane_index) const;

    /// @brief 获取视频元数据（若 type 为 VIDEO）
    const VideoFrameMeta* VideoMeta() const {
        if (type == MediaType::VIDEO) {
            return &std::get<VideoFrameMeta>(meta);
        }
        return nullptr;
    }

    /// @brief 获取音频元数据（若 type 为 AUDIO）
    const AudioFrameMeta* AudioMeta() const {
        if (type == MediaType::AUDIO) {
            return &std::get<AudioFrameMeta>(meta);
        }
        return nullptr;
    }

    /// @brief 获取视频宽度
    int32_t Width() const {
        if (auto* v = VideoMeta()) return v->width;
        return 0;
    }

    /// @brief 获取视频高度
    int32_t Height() const {
        if (auto* v = VideoMeta()) return v->height;
        return 0;
    }

    /// @brief 获取像素格式
    ::PixelFormat PixelFormat() const {
        if (auto* v = VideoMeta()) return v->pixel_format;
        return PixelFormat::kUnknown;
    }

    /// @brief 获取平面数量
    int32_t PlaneCount() const {
        if (auto* v = VideoMeta()) return v->plane_count;
        if (auto* a = AudioMeta()) return a->plane_count;
        return 0;
    }

    /// @brief 获取指定平面的 stride
    int32_t Stride(int index) const {
        if (index < 0 || index >= PlaneCount()) {
            return 0;
        }
        if (auto* v = VideoMeta()) return v->plane_info[index].stride;
        if (auto* a = AudioMeta()) return a->planes[index].stride;
        return 0;
    }

    /// @brief 获取指定平面的 offset
    int32_t PlaneOffset(int index) const {
        if (index < 0 || index >= PlaneCount()) {
            return 0;
        }
        if (auto* v = VideoMeta()) return v->plane_info[index].offset;
        if (auto* a = AudioMeta()) return a->planes[index].offset;
        return 0;
    }

    /// @brief 获取样本数（音频）
    int NbSamples() const {
        if (auto* a = AudioMeta()) return a->nb_samples;
        return 0;
    }

    /// @brief 获取采样率（音频）
    int SampleRate() const {
        if (auto* a = AudioMeta()) return a->sample_rate;
        return 0;
    }

    /// @brief 获取通道数（音频）
    int Channels() const {
        if (auto* a = AudioMeta()) return a->channels;
        return 0;
    }

    /// @brief 获取采样格式（音频）
    SampleFormat SampleFmt() const {
        if (auto* a = AudioMeta()) return a->sample_format;
        return SampleFormat::Unknown;
    }
};