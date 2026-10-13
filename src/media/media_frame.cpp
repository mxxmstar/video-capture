#include "media/media_frame.h"
#include "media/simple_buffer.h"
#include "media/simple_buffer_view.h"

#include <cstring>
#include <limits>
#include <vector>

SourcePlaneLayout SourcePlaneLayout::Compute(PixelFormat format, int width, int height, int source_stride) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || source_stride <= 0) {
        LOG_ERROR("Invalid capture dimensions: {}x{}, stride={}", width, height, source_stride);
        return {};
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto s = static_cast<std::size_t>(source_stride);

    SourcePlaneLayout layout{};

    if (format == ::PixelFormat::kYUYV422) {
        if (w % 2 != 0) {
            LOG_ERROR("YUYV width must be even");
            return layout;            
        }
        layout.plane_count = 1;
        layout.planes[0] = {static_cast<int>(h), static_cast<int>(w * 2), source_stride, 0};
    } else if (format == ::PixelFormat::kNV12 || format == ::PixelFormat::kNV21 || format == ::PixelFormat::kI420) {
        if (w % 2 != 0 || h % 2 != 0 || (format == ::PixelFormat::kI420 && s % 2 != 0)) {
            LOG_ERROR("Invalid 4:2:0 capture alignment: {}x{}, stride={}", width, height, source_stride);
            return {};
        }
        layout.plane_count = (format == ::PixelFormat::kNV12 || format == ::PixelFormat::kNV21) ? 2 : 3;
        // Y 平面
        layout.planes[0] = {static_cast<int>(h), static_cast<int>(w), source_stride, 0};

        // UV 平面
        const int uv_rows = static_cast<int>(h / 2);
        if (layout.plane_count == 2) {
            // NV12: UV 平面和 Y 平面的 stride 相同，row_bytes = width   
            layout.planes[1] = {uv_rows, static_cast<int>(w), source_stride, s * h};
        } else {
            // I420: U 平面和 V 平面的 stride 为 width / 2，row_bytes = width / 2
            const int half_stride = source_stride / 2;
            layout.planes[1] = {uv_rows, static_cast<int>(w / 2), half_stride, s * h};
            layout.planes[2] = {uv_rows, static_cast<int>(w / 2), half_stride,
                                s * h + static_cast<std::size_t>(half_stride) * static_cast<std::size_t>(uv_rows)};
        }
    } else {
        LOG_ERROR("Unsupported capture pixel format: {}", static_cast<int>(format));
        return {};
    }

    return layout;
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(std::shared_ptr<const uint8_t> source_data, std::size_t source_bytes,
    ::PixelFormat format, int width, int height, int source_stride) {

    if (!source_data) {
        LOG_ERROR("CreateCompact: null source data");
        return nullptr;
    }

    const auto layout = SourcePlaneLayout::Compute(format, width, height, source_stride);
    if (layout.is_empty()) {
        return nullptr; // Compute() 已经打印了错误信息
    }

    VideoFrameMeta meta;
    meta.width = width;
    meta.height = height;
    meta.pixel_format = format;
    meta.plane_count = layout.plane_count;

    std::size_t total = 0;
    for (int p = 0; p < layout.plane_count; ++p) {
        const auto& pl = layout.planes[p];
        const std::size_t size = static_cast<std::size_t>(pl.row_bytes) * static_cast<std::size_t>(pl.rows);
        const auto src_stride = static_cast<std::size_t>(pl.source_stride);
        const auto rbytes = static_cast<std::size_t>(pl.row_bytes);

        if (src_stride < rbytes || pl.source_offset > source_bytes ||
            (static_cast<std::size_t>(pl.rows) - 1) * src_stride + rbytes > source_bytes - pl.source_offset ||
            size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) - total) {
            LOG_ERROR("CreateCompact: plane {} payload truncated or layout invalid", p);
            return nullptr;
        }

        meta.plane_info[p] = {static_cast<int32_t>(total), static_cast<int32_t>(pl.row_bytes), static_cast<int32_t>(size)};
        total += size;
    }

    // 零拷贝判断：布局已是紧凑格式（各 plane stride==row_bytes 且平面间连续）
    bool compact_layout = true;
    for (int p = 0; p < layout.plane_count; ++p) {
        // 检查各平面 stride 是否等于 row_bytes
        if (layout.planes[p].source_stride != layout.planes[p].row_bytes) {
            compact_layout = false;
            break;
        }
    }
    if (compact_layout) {
        // 检查平面间是否连续
        for (int p = 1; p < layout.plane_count; ++p) {
            const auto& prev = layout.planes[p - 1];
            const auto& curr = layout.planes[p];
            if (curr.source_offset != prev.source_offset +
                static_cast<std::size_t>(prev.rows) * static_cast<std::size_t>(prev.source_stride)) {
                compact_layout = false;
                break;
            }
        }
    }

    auto frame = std::make_shared<MediaFrame>();
    frame->meta = meta;

    if (compact_layout) {
        frame->buffer = std::make_shared<SimpleBufferView>(std::move(source_data), total);
    } else {
        std::vector<uint8_t> packed(total);
        const uint8_t* src = source_data.get();
        for (int p = 0; p < layout.plane_count; ++p) {
            const auto& pl = layout.planes[p];
            for (std::size_t row = 0; row < static_cast<std::size_t>(pl.rows); ++row) {
                // 拷贝每行有效字节，去除 stride 填充
                std::memcpy(packed.data() + meta.plane_info[p].offset + row * static_cast<std::size_t>(pl.row_bytes),
                    src + pl.source_offset + row * static_cast<std::size_t>(pl.source_stride),
                    static_cast<std::size_t>(pl.row_bytes));
            }
        }
        frame->buffer = std::make_shared<SimpleBuffer>(std::move(packed));
    }

    frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
    return frame;
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(const unsigned char* source_data, std::size_t source_bytes, ::PixelFormat format,
    int width, int height, int source_stride) {

    if (!source_data) {
        LOG_ERROR("CreateCompact: null source data");
        return nullptr;
    }

    const auto layout = SourcePlaneLayout::Compute(format, width, height, source_stride);
    if (layout.is_empty()) {
        return nullptr;
    }

    VideoFrameMeta meta;
    meta.width = width;
    meta.height = height;
    meta.pixel_format = format;
    meta.plane_count = layout.plane_count;

    std::size_t total = 0;
    for (int p = 0; p < layout.plane_count; ++p) {
        const auto& pl = layout.planes[p];
        const std::size_t size = static_cast<std::size_t>(pl.row_bytes) * static_cast<std::size_t>(pl.rows);
        const auto src_stride = static_cast<std::size_t>(pl.source_stride);
        const auto rbytes = static_cast<std::size_t>(pl.row_bytes);

        if (src_stride < rbytes || pl.source_offset > source_bytes ||
            (static_cast<std::size_t>(pl.rows) - 1) * src_stride + rbytes > source_bytes - pl.source_offset ||
            size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) - total) {
            LOG_ERROR("CreateCompact: plane {} payload truncated or layout invalid", p);
            return nullptr;
        }

        meta.plane_info[p] = {static_cast<int32_t>(total), static_cast<int32_t>(pl.row_bytes), static_cast<int32_t>(size)};
        total += size;
    }

    std::vector<uint8_t> packed(total);
    for (int p = 0; p < layout.plane_count; ++p) {
        const auto& pl = layout.planes[p];
        for (std::size_t row = 0; row < static_cast<std::size_t>(pl.rows); ++row) {
            std::memcpy(packed.data() + meta.plane_info[p].offset + row * static_cast<std::size_t>(pl.row_bytes),
                source_data + pl.source_offset + row * static_cast<std::size_t>(pl.source_stride),
                static_cast<std::size_t>(pl.row_bytes));
        }
    }

    auto frame = std::make_shared<MediaFrame>();
    frame->meta = meta;
    frame->buffer = std::make_shared<SimpleBuffer>(std::move(packed));
    frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
    return frame;
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(const unsigned char* source_data, std::size_t source_bytes, ::PixelFormat format,
    int width, int height, int source_stride, uint8_t* dest, std::size_t dest_capacity) {

    if (!source_data || !dest) {
        LOG_ERROR("CreateCompact: null source data or dest");
        return nullptr;
    }

    const auto layout = SourcePlaneLayout::Compute(format, width, height, source_stride);
    if (layout.is_empty()) {
        return nullptr;
    }

    VideoFrameMeta meta;
    meta.width = width;
    meta.height = height;
    meta.pixel_format = format;
    meta.plane_count = layout.plane_count;

    std::size_t total = 0;
    for (int p = 0; p < layout.plane_count; ++p) {
        const auto& pl = layout.planes[p];
        const std::size_t size = static_cast<std::size_t>(pl.row_bytes) * static_cast<std::size_t>(pl.rows);
        const auto src_stride = static_cast<std::size_t>(pl.source_stride);
        const auto rbytes = static_cast<std::size_t>(pl.row_bytes);

        if (src_stride < rbytes || pl.source_offset > source_bytes ||
            (static_cast<std::size_t>(pl.rows) - 1) * src_stride + rbytes > source_bytes - pl.source_offset ||
            size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) - total) {
            LOG_ERROR("CreateCompact: plane {} payload truncated or layout invalid", p);
            return nullptr;
        }

        meta.plane_info[p] = {static_cast<int32_t>(total), static_cast<int32_t>(pl.row_bytes), static_cast<int32_t>(size)};
        total += size;
    }

    if (total > dest_capacity) {
        LOG_ERROR("CreateCompact: dest capacity {} insufficient, need {}", dest_capacity, total);
        return nullptr;
    }

    for (int p = 0; p < layout.plane_count; ++p) {
        const auto& pl = layout.planes[p];
        for (std::size_t row = 0; row < static_cast<std::size_t>(pl.rows); ++row) {
            std::memcpy(dest + meta.plane_info[p].offset + row * static_cast<std::size_t>(pl.row_bytes),
                source_data + pl.source_offset + row * static_cast<std::size_t>(pl.source_stride),
                static_cast<std::size_t>(pl.row_bytes));
        }
    }

    // 非拥有式 aliasing shared_ptr：不管理 dest 生命周期，仅用于 SimpleBufferView 的类型约束
    auto dest_ptr = std::shared_ptr<uint8_t>(std::shared_ptr<void>{}, dest);
    auto frame = std::make_shared<MediaFrame>();
    frame->meta = meta;
    frame->buffer = std::make_shared<SimpleBufferView>(std::move(dest_ptr), total);
    frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
    return frame;
}

PlaneSlice MediaFrame::GetPlaneSlice(int plane_index) const {
    const auto* vm = VideoMeta();
    if (!vm || plane_index < 0 || plane_index >= vm->plane_count) {
        return {};
    }

    const auto* base = buffer ? buffer->Data() : nullptr;
    const std::size_t size = buffer ? buffer->Size() : 0;
    if (!base || size == 0) {
        return {};
    }

    const int32_t offset = vm->plane_info[plane_index].offset;
    const int32_t stride = vm->plane_info[plane_index].stride;
    const int32_t plane_size = vm->plane_info[plane_index].size;

    if (offset < 0 || stride <= 0 || plane_size <= 0) {
        return {};
    }
    if (static_cast<std::size_t>(offset) + static_cast<std::size_t>(plane_size) > size) {
        return {};
    }

    int row_bytes = stride;
    int rows = 0;

    switch (vm->pixel_format) {
        case ::PixelFormat::kYUYV422:
            row_bytes = vm->width * 2;
            rows = vm->height;
            break;
        case ::PixelFormat::kNV12:
        case ::PixelFormat::kNV21:
            if (plane_index == 0) {
                row_bytes = vm->width;
                rows = vm->height;
            } else {
                row_bytes = vm->width;
                rows = vm->height / 2;
            }
            break;
        case ::PixelFormat::kI420:
            if (plane_index == 0) {
                row_bytes = vm->width;
                rows = vm->height;
            } else {
                row_bytes = vm->width / 2;
                rows = vm->height / 2;
            }
            break;
        default:
            return {};
    }

    return {base + offset, stride, row_bytes, rows};
}