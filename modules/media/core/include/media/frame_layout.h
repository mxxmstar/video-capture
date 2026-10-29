#pragma once

#include "media/media_buffer.h"

#include <cstddef>
#include <vector>

enum class PixelFormat;


/// @brief 平面布局条目
struct PlaneLayoutEntry {
    PlaneGeometry geometry;
    std::size_t source_offset = 0;
};

/// @brief 平面布局
struct PlaneLayout {
    std::vector<PlaneLayoutEntry> planes;

    bool IsEmpty() const noexcept { return planes.empty(); }

    /// @brief 根据像素格式、宽度、高度和源步长计算平面布局
    /// @param format 像素格式
    /// @param width 宽度
    /// @param height 高度
    /// @param source_stride 源步长
    /// @return 平面布局
    static PlaneLayout Compute(PixelFormat format, int width, int height, int source_stride);
};
