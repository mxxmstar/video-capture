#include "media/media_frame.h"

#include <cstring>
#include <limits>
#include <vector>
#include <cassert>
namespace {

bool BuildPackedOffsets(const PlaneLayout& layout, std::vector<std::size_t>& offsets,
                        std::size_t& total) {
    offsets.clear();
    offsets.reserve(layout.planes.size());
    total = 0;
    for (const auto& plane : layout.planes) {
        const auto& geometry = plane.geometry;
        if (geometry.row_bytes == 0 || geometry.rows == 0 ||
            geometry.row_bytes > std::numeric_limits<std::size_t>::max() / geometry.rows) {
            return false;
        }
        const auto size = geometry.row_bytes * geometry.rows;
        if (size > std::numeric_limits<std::size_t>::max() - total) return false;
        offsets.push_back(total);
        total += size;
    }
    return !layout.planes.empty();
}

bool ValidateSourceLayout(const PlaneLayout& layout, std::size_t source_bytes) {
    for (const auto& plane : layout.planes) {
        const auto& geometry = plane.geometry;
        if (geometry.stride <= 0 || geometry.row_bytes == 0 || geometry.rows == 0 ||
            geometry.row_bytes > static_cast<std::size_t>(geometry.stride) ||
            plane.source_offset > source_bytes) {
            return false;
        }
        const auto stride = static_cast<std::size_t>(geometry.stride);
        if (geometry.rows - 1 > (std::numeric_limits<std::size_t>::max() - geometry.row_bytes) / stride) {
            return false;
        }
        const auto extent = (geometry.rows - 1) * stride + geometry.row_bytes;
        if (extent > source_bytes - plane.source_offset) return false;
    }
    return !layout.planes.empty();
}

bool IsAlreadyCompact(const PlaneLayout& layout) {
    std::size_t expected_offset = 0;
    for (const auto& plane : layout.planes) {
        const auto& geometry = plane.geometry;
        if (geometry.stride != static_cast<std::ptrdiff_t>(geometry.row_bytes) ||
            plane.source_offset != expected_offset ||
            geometry.row_bytes > std::numeric_limits<std::size_t>::max() / geometry.rows) {
            return false;
        }
        expected_offset += geometry.row_bytes * geometry.rows;
    }
    return true;
}

std::vector<MediaPlaneView> MakeCompactViews(const uint8_t* base, const PlaneLayout& layout,
                                            const std::vector<std::size_t>& offsets) {
    std::vector<MediaPlaneView> views;
    views.reserve(layout.planes.size());
    for (std::size_t i = 0; i < layout.planes.size(); ++i) {
        const auto& geometry = layout.planes[i].geometry;
        const auto size = geometry.row_bytes * geometry.rows;
        views.emplace_back(base + offsets[i], size,
            PlaneGeometry{static_cast<std::ptrdiff_t>(geometry.row_bytes),
                geometry.row_bytes, geometry.rows});
    }
    return views;
}

void CopyCompact(const uint8_t* source, const PlaneLayout& layout,
                 const std::vector<std::size_t>& offsets, uint8_t* destination) {
    for (std::size_t i = 0; i < layout.planes.size(); ++i) {
        const auto& entry = layout.planes[i];
        const auto& geometry = entry.geometry;
        for (std::size_t row = 0; row < geometry.rows; ++row) {
            std::memcpy(destination + offsets[i] + row * geometry.row_bytes,
                source + entry.source_offset + row * static_cast<std::size_t>(geometry.stride),
                geometry.row_bytes);
        }
    }
}

std::shared_ptr<MediaFrame> MakeFrame(const PlaneLayout& layout, int width, int height,
                                     PixelFormat format, std::vector<uint8_t> packed) {
    std::vector<std::size_t> offsets;
    std::size_t total = 0;
    if (!BuildPackedOffsets(layout, offsets, total) || packed.size() != total) return nullptr;

    auto frame = std::make_shared<MediaFrame>();
    VideoFrameMeta meta{};
    meta.width = width;
    meta.height = height;
    meta.pixel_format = format;
    frame->meta = meta;
    auto owner = std::make_shared<std::vector<uint8_t>>(std::move(packed));
    const auto* bytes = owner->data();
    auto views = MakeCompactViews(bytes, layout, offsets);
    frame->view = MediaBufferView::FromContiguous(std::move(views),
        ConstBufferRegion{bytes, total}, std::move(owner));
    frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
    return frame;
}

} // namespace

PlaneLayout PlaneLayout::Compute(PixelFormat format, int width, int height, int source_stride) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || source_stride <= 0) {
        LOG_ERROR("Invalid capture dimensions: {}x{}, stride={}", width, height, source_stride);
        return {};
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto stride = static_cast<std::size_t>(source_stride);
    PlaneLayout layout;

    if (format == PixelFormat::kYUYV422) {
        if (w % 2 != 0) {
            LOG_ERROR("YUYV width must be even");
            return {};
        }
        layout.planes.push_back({PlaneGeometry{source_stride, w * 2, h}, 0});
    } else if (format == PixelFormat::kNV12 || format == PixelFormat::kNV21 ||
               format == PixelFormat::kI420) {
        if (w % 2 != 0 || h % 2 != 0 ||
            (format == PixelFormat::kI420 && stride % 2 != 0)) {
            LOG_ERROR("Invalid 4:2:0 capture alignment: {}x{}, stride={}", width, height, source_stride);
            return {};
        }
        layout.planes.push_back({PlaneGeometry{source_stride, w, h}, 0});
        const auto chroma_rows = h / 2;
        const auto chroma_offset = stride * h;
        if (format == PixelFormat::kNV12 || format == PixelFormat::kNV21) {
            layout.planes.push_back({PlaneGeometry{source_stride, w, chroma_rows}, chroma_offset});
        } else {
            const auto chroma_stride = stride / 2;
            layout.planes.push_back({PlaneGeometry{static_cast<std::ptrdiff_t>(chroma_stride), w / 2,
                chroma_rows}, chroma_offset});
            layout.planes.push_back({PlaneGeometry{static_cast<std::ptrdiff_t>(chroma_stride), w / 2,
                chroma_rows}, chroma_offset + chroma_stride * chroma_rows});
        }
    } else {
        LOG_ERROR("Unsupported capture pixel format: {}", static_cast<int>(format));
        return {};
    }

    for (const auto& plane : layout.planes) {
        if (plane.geometry.stride <= 0 ||
            plane.geometry.row_bytes > static_cast<std::size_t>(plane.geometry.stride)) {
            LOG_ERROR("Capture stride is smaller than plane row size");
            return {};
        }
    }
    return layout;
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(std::shared_ptr<const uint8_t> source_data,
    std::size_t source_bytes, ::PixelFormat format, int width, int height, int source_stride) {
    if (!source_data) {
        LOG_ERROR("CreateCompact: null source data");
        return nullptr;
    }
    const auto layout = PlaneLayout::Compute(format, width, height, source_stride);
    std::vector<std::size_t> offsets;
    std::size_t total = 0;
    if (layout.IsEmpty() || !ValidateSourceLayout(layout, source_bytes) ||
        !BuildPackedOffsets(layout, offsets, total)) {
        LOG_ERROR("CreateCompact: source layout is invalid or truncated");
        return nullptr;
    }

    if (IsAlreadyCompact(layout)) {
        auto frame = std::make_shared<MediaFrame>();
        VideoFrameMeta meta{};
        meta.width = width;
        meta.height = height;
        meta.pixel_format = format;
        frame->meta = meta;
        auto owner = std::move(source_data);
        const auto* bytes = owner.get();
        auto views = MakeCompactViews(bytes, layout, offsets);
        frame->view = MediaBufferView::FromContiguous(std::move(views),
            ConstBufferRegion{bytes, total}, std::move(owner));
        frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
        return frame;
    }

    std::vector<uint8_t> packed(total);
    CopyCompact(source_data.get(), layout, offsets, packed.data());
    return MakeFrame(layout, width, height, format, std::move(packed));
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(const unsigned char* source_data,
    std::size_t source_bytes, ::PixelFormat format, int width, int height, int source_stride) {
    if (!source_data) {
        LOG_ERROR("CreateCompact: null source data");
        return nullptr;
    }
    const auto layout = PlaneLayout::Compute(format, width, height, source_stride);
    std::vector<std::size_t> offsets;
    std::size_t total = 0;
    if (layout.IsEmpty() || !ValidateSourceLayout(layout, source_bytes) ||
        !BuildPackedOffsets(layout, offsets, total)) {
        LOG_ERROR("CreateCompact: source layout is invalid or truncated");
        return nullptr;
    }
    std::vector<uint8_t> packed(total);
    CopyCompact(source_data, layout, offsets, packed.data());
    return MakeFrame(layout, width, height, format, std::move(packed));
}

std::shared_ptr<MediaFrame> MediaFrame::CreateCompact(const unsigned char* source_data,
    std::size_t source_bytes, ::PixelFormat format, int width, int height, int source_stride,
    uint8_t* dest, std::size_t dest_capacity) {
    if (!source_data || !dest) {
        LOG_ERROR("CreateCompact: null source data or dest");
        return nullptr;
    }
    const auto layout = PlaneLayout::Compute(format, width, height, source_stride);
    std::vector<std::size_t> offsets;
    std::size_t total = 0;
    if (layout.IsEmpty() || !ValidateSourceLayout(layout, source_bytes) ||
        !BuildPackedOffsets(layout, offsets, total)) {
        LOG_ERROR("CreateCompact: source layout is invalid or truncated");
        return nullptr;
    }
    if (total > dest_capacity) {
        LOG_ERROR("CreateCompact: dest capacity {} insufficient, need {}", dest_capacity, total);
        return nullptr;
    }
    CopyCompact(source_data, layout, offsets, dest);

    auto frame = std::make_shared<MediaFrame>();
    VideoFrameMeta meta{};
    meta.width = width;
    meta.height = height;
    meta.pixel_format = format;
    frame->meta = meta;
    auto views = MakeCompactViews(dest, layout, offsets);
    frame->view = MediaBufferView::FromContiguous(std::move(views),
        ConstBufferRegion{dest, total});
    frame->time = {kNoTimestamp, kNoTimestamp, kNoTimestamp};
    return frame;
}

MediaPlaneView MediaFrame::PlaneView(int plane_index) const {
    if (plane_index < 0) return {};
    const auto* plane = view.Plane(static_cast<std::size_t>(plane_index));
    return plane && plane->IsValid() ? *plane : MediaPlaneView{};
}
