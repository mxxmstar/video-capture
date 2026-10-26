#pragma once

#include "media/media_buffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

namespace Media {
// The owner must retain this exact AVFrame and its buffers.
inline MediaBufferView MakeFFmpegFrameView(const AVFrame& frame, std::shared_ptr<const void> owner) {
    std::vector<MediaPlaneView> planes;
    if (frame.width > 0 && frame.height > 0) {
        const auto format = static_cast<AVPixelFormat>(frame.format);
        const int count = av_pix_fmt_count_planes(format);
        int row_bytes[4]{};
        ptrdiff_t compact_strides[4]{};
        size_t compact_sizes[4]{};
        if (count <= 0 || count > 4 || av_image_fill_linesizes(row_bytes, format, frame.width) < 0) return {};
        for (int p = 0; p < 4; ++p) compact_strides[p] = row_bytes[p];
        if (av_image_fill_plane_sizes(compact_sizes, format, frame.height, compact_strides) < 0) return {};
        for (int p = 0; p < count; ++p) {
            if (row_bytes[p] <= 0 || compact_sizes[p] == 0 || !frame.data[p] || frame.linesize[p] == 0) return {};
            const auto rows = compact_sizes[p] / static_cast<size_t>(row_bytes[p]);
            const auto stride = static_cast<ptrdiff_t>(frame.linesize[p]);
            const auto magnitude = stride < 0 ? static_cast<size_t>(-(stride + 1)) + 1 : static_cast<size_t>(stride);
            if (magnitude < static_cast<size_t>(row_bytes[p]) || rows == 0 ||
                rows - 1 > (static_cast<size_t>(PTRDIFF_MAX) - row_bytes[p]) / magnitude) return {};
            planes.emplace_back(frame.data[p], (rows - 1) * magnitude + row_bytes[p],
                PlaneGeometry{stride, static_cast<size_t>(row_bytes[p]), rows});
        }
    } else if (frame.nb_samples > 0 && frame.ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(frame.format);
        const int bytes = av_get_bytes_per_sample(format);
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        if (bytes <= 0 || !frame.extended_data) return {};
        const int count = planar ? frame.ch_layout.nb_channels : 1;
        const auto size = static_cast<size_t>(frame.nb_samples) * bytes * (planar ? 1 : frame.ch_layout.nb_channels);
        if (size > static_cast<size_t>(PTRDIFF_MAX)) return {};
        for (int p = 0; p < count; ++p) {
            if (!frame.extended_data[p]) return {};
            planes.emplace_back(frame.extended_data[p], size, PlaneGeometry{static_cast<ptrdiff_t>(size), size, 1});
        }
    }
    return MediaBufferView(std::move(planes), std::move(owner));
}
} // namespace Media
