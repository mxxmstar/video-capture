#pragma once

#include <cassert>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>


struct PlaneGeometry {
    std::ptrdiff_t stride = 0;
    std::size_t row_bytes = 0;
    std::size_t rows = 0;
};

/// @brief 缓冲区
struct BufferRegion {
    uint8_t* data = nullptr;
    std::size_t size = 0;

    std::span<uint8_t> Bytes() const noexcept { return {data, size}; }
};

/// @brief 只读缓冲区区域
struct ConstBufferRegion {
    const uint8_t* data = nullptr;
    std::size_t size = 0;

    std::span<const uint8_t> Bytes() const noexcept { return {data, size}; }
};

struct MediaPlaneView {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    PlaneGeometry geometry;

    MediaPlaneView() = default;
    MediaPlaneView(const uint8_t* src, std::size_t src_size, PlaneGeometry g)
        : data(src), size(src_size), geometry(g) {}

    bool IsValid() const noexcept {
        if (!data || geometry.stride == 0 || geometry.rows == 0 ||
            geometry.row_bytes == 0) {
            return false;
        }
        const auto magnitude = geometry.stride < 0
            ? static_cast<std::size_t>(-(geometry.stride + 1)) + 1
            : static_cast<std::size_t>(geometry.stride);
        if (magnitude < geometry.row_bytes || size > static_cast<std::size_t>(
                std::numeric_limits<std::ptrdiff_t>::max())) {
            return false;
        }
        if (geometry.rows - 1 > (size - std::min(size, geometry.row_bytes)) / magnitude) {
            return false;
        }
        return geometry.row_bytes <= size;
    }
};

class MediaBufferView;

class MediaBuffer {
public:
    MediaBuffer() = default;
    MediaBuffer(const MediaBuffer&) = delete;
    MediaBuffer& operator=(const MediaBuffer&) = delete;
    MediaBuffer(MediaBuffer&&) noexcept = default;
    MediaBuffer& operator=(MediaBuffer&&) noexcept = default;

    /// @brief 添加一个平面到缓冲区
    /// @param data 平面数据指针
    /// @param size 平面数据大小
    /// @param stride 平面步长
    /// @param rows 平面行数
    void PushPlane(const uint8_t* data, std::size_t size, std::ptrdiff_t stride, std::size_t rows) {
        const auto row_bytes = stride >= 0 ? static_cast<std::size_t>(stride) : 0;
        PushPlane(data, size, PlaneGeometry{stride, row_bytes, rows});
    }

    /// @brief 添加一个平面到缓冲区
    /// @param data 平面数据指针
    /// @param size 平面数据大小
    /// @param geometry 平面几何信息
    void PushPlane(const uint8_t* data, std::size_t size, PlaneGeometry geometry) {
        assert(data != nullptr || size == 0);
        // The source may alias existing storage; retain it before changing allocations.
        std::vector<uint8_t> incoming;
        if (size != 0) incoming.assign(data, data + size);
        planes_.reserve(planes_.size() + 1);
        if (has_contiguous_storage_) {
            std::vector<std::vector<uint8_t>> separate;
            separate.reserve(planes_.size() + 1);
            for (const auto& plane : planes_) {
                separate.emplace_back(plane.data, plane.data + plane.size);
            }
            separate.push_back(std::move(incoming));
            separate_storage_ = std::move(separate);
            for (std::size_t i = 0; i < planes_.size(); ++i)
                planes_[i].data = separate_storage_[i].data();
            contiguous_storage_.clear();
            has_contiguous_storage_ = false;
        } else {
            separate_storage_.push_back(std::move(incoming));
        }
        planes_.emplace_back(separate_storage_.back().data(), size, geometry);
    }

    /// @brief 添加一个平面到缓冲区
    /// @param data 平面数据指针
    /// @param size 平面数据大小
    /// @param stride 平面步长
    /// @param rows 平面行数
    void Push(const uint8_t* data, std::size_t size, std::ptrdiff_t stride, std::size_t rows) {
        PushPlane(data, size, stride, rows);
    }

    /// @brief 添加一个平面到缓冲区
    /// @param data 平面数据指针
    /// @param size 平面数据大小
    /// @param geometry 平面几何信息
    void Push(const uint8_t* data, std::size_t size, PlaneGeometry geometry) {
        PushPlane(data, size, geometry);
    }

    std::size_t PlaneCount() const noexcept { return planes_.size(); }

    /// @brief 获取指定平面的可读写数据指针
    /// @param index 平面索引
    /// @return 平面数据指针
    uint8_t* Data(std::size_t index) noexcept {
        // All planes in MediaBuffer refer to its own mutable allocations.
        return index < planes_.size() ? const_cast<uint8_t*>(planes_[index].data) : nullptr;
    }
    const uint8_t* Data(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].data : nullptr;
    }
    std::span<uint8_t> Plane(std::size_t index) noexcept {
        return index < planes_.size() ? std::span<uint8_t>(Data(index), PlaneSize(index)) : std::span<uint8_t>{};
    }
    
    /// @brief 获取指定平面的只读数据视图
    /// @param index 平面索引
    /// @return 平面数据视图
    std::span<const uint8_t> Plane(std::size_t index) const noexcept {
        return index < planes_.size() ? std::span<const uint8_t>(Data(index), PlaneSize(index)) : std::span<const uint8_t>{};
    }

    /// @brief 获取指定平面的大小
    /// @param index 平面索引
    /// @return 平面数据大小
    std::size_t PlaneSize(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].size : 0;
    }
    
    /// @brief 获取指定平面的几何信息
    /// @param index 平面索引
    /// @return 平面几何信息
    PlaneGeometry Geometry(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].geometry : PlaneGeometry{};
    }

    /// @brief 获取缓冲区的有效数据字节数
    /// @return 缓冲区有效数据字节数
    std::size_t TotalSize() const noexcept {
        if (has_contiguous_storage_) return contiguous_storage_.size();
        std::size_t total = 0;
        for (const auto& plane : planes_) total += plane.size;
        return total;
    }

    /// @brief 是否为一段连续存储
    bool IsContiguous() const noexcept {
        return has_contiguous_storage_ || planes_.size() <= 1;
    }

    /// @brief 获取缓冲区的连续数据视图
    std::optional<std::span<uint8_t>> ContiguousBytes() noexcept {
        if (has_contiguous_storage_) return std::span<uint8_t>(contiguous_storage_);
        if (planes_.size() == 1) return Plane(0);
        return std::nullopt;
    }

    /// @brief 获取缓冲区的连续只读数据视图
    std::optional<std::span<const uint8_t>> ContiguousBytes() const noexcept {
        if (has_contiguous_storage_) return std::span<const uint8_t>(contiguous_storage_);
        if (planes_.size() == 1) return Plane(0);
        return std::nullopt;
    }

    
    /// @brief 获取缓冲区的可变写入区域
    /// @return 可变写入区域
    std::vector<BufferRegion> MutableRegions() noexcept {
        if (has_contiguous_storage_) {
            return {{contiguous_storage_.data(), contiguous_storage_.size()}};
        }
        std::vector<BufferRegion> regions;
        regions.reserve(planes_.size());
        for (std::size_t i = 0; i < planes_.size(); ++i) {
            regions.push_back({Data(i), PlaneSize(i)});
        }
        return regions;
    }
    std::vector<ConstBufferRegion> MemoryRegions() const {
        if (has_contiguous_storage_) {
            return {{contiguous_storage_.data(), contiguous_storage_.size()}};
        }
        std::vector<ConstBufferRegion> regions;
        regions.reserve(planes_.size());
        for (std::size_t i = 0; i < planes_.size(); ++i) {
            regions.push_back({Data(i), PlaneSize(i)});
        }
        return regions;
    }

    static std::shared_ptr<MediaBuffer> CopyContiguous(const MediaBufferView& source);
    static std::shared_ptr<MediaBuffer> CopySeparatePlanes(const MediaBufferView& source);

private:
    friend class MediaBufferView;

    // 平面描述
    std::vector<MediaPlaneView> planes_;
    std::vector<uint8_t> contiguous_storage_;
    std::vector<std::vector<uint8_t>> separate_storage_;
    bool has_contiguous_storage_ = false;
};

class MediaBufferView {
public:
    MediaBufferView() = default;
    explicit MediaBufferView(std::vector<MediaPlaneView> planes) : planes_(std::move(planes)) {
        if (planes_.size() == 1) contiguous_region_ = {planes_[0].data, planes_[0].size};
    }

    MediaBufferView(std::vector<MediaPlaneView> planes, std::shared_ptr<const void> owner) : MediaBufferView(std::move(planes)) {
        owner_ = std::move(owner);
    }

    /// @brief 从连续存储的平面视图创建 MediaBufferView
    /// @param planes 平面视图
    /// @param region 有效数据区域
    /// @param owner 所有者指针    
    static MediaBufferView FromContiguous(std::vector<MediaPlaneView> planes, ConstBufferRegion region, std::shared_ptr<const void> owner = {}) {
        if ((!region.data && region.size != 0) || region.size >
                std::numeric_limits<std::uintptr_t>::max() -
                    reinterpret_cast<std::uintptr_t>(region.data)) {
            return {};
        }
        const auto region_start = reinterpret_cast<std::uintptr_t>(region.data);
        for (const auto& plane : planes) {
            if (!plane.IsValid()) return {};
            const auto magnitude = plane.geometry.stride < 0
                ? static_cast<std::size_t>(-(plane.geometry.stride + 1)) + 1
                : static_cast<std::size_t>(plane.geometry.stride);
            const auto row_offset = (plane.geometry.rows - 1) * magnitude;  // 最后一行偏移量
            const auto logical_row_zero = reinterpret_cast<std::uintptr_t>(plane.data);
            if ((plane.geometry.stride < 0 && logical_row_zero < row_offset) ||
                (plane.geometry.stride >= 0 && row_offset > std::numeric_limits<std::uintptr_t>::max() - logical_row_zero)) {
                return {};
            }
            const auto plane_start = plane.geometry.stride < 0 ? logical_row_zero - row_offset : logical_row_zero;
            if (plane_start < region_start) return {};
            const auto plane_offset = plane_start - region_start;
            const auto extent = row_offset + plane.geometry.row_bytes;
            if (plane_offset > region.size || extent > region.size - plane_offset) {
                return {};
            }
        }
        MediaBufferView view;
        view.planes_ = std::move(planes);
        view.contiguous_region_ = region;
        view.owner_ = std::move(owner);
        return view;
    }

    /// @brief 从平面视图创建 MediaBufferView
    /// @param planes 平面视图
    /// @param owner 所有者指针    
    static MediaBufferView FromPlanes(std::vector<MediaPlaneView> planes, std::shared_ptr<const void> owner = {}) {
        return MediaBufferView(std::move(planes), std::move(owner));
    }

    /// @brief 从 MediaBuffer 创建 MediaBufferView
    /// @param owner MediaBuffer 指针
    /// @return MediaBufferView
    static MediaBufferView FromOwner(std::shared_ptr<const MediaBuffer> owner) {
        std::vector<MediaPlaneView> planes;
        if (owner) {
            planes.reserve(owner->planes_.size());
            for (std::size_t i = 0; i < owner->planes_.size(); ++i) {
                planes.emplace_back(owner->Data(i), owner->PlaneSize(i), owner->Geometry(i));
            }
        }
        MediaBufferView view(std::move(planes), owner);
        if (owner && owner->has_contiguous_storage_) {
            view.contiguous_region_ = {owner->contiguous_storage_.data(), owner->contiguous_storage_.size()};
        }
        return view;
    }

    std::size_t PlaneCount() const noexcept { return planes_.size(); }
    std::span<const uint8_t> Data(std::size_t index) const noexcept {
        if (index >= planes_.size()) return {};
        const auto& plane = planes_[index];
        if (!plane.IsValid()) return {};
        const auto* begin = plane.geometry.stride < 0
            ? plane.data + static_cast<std::ptrdiff_t>(plane.geometry.rows - 1) * plane.geometry.stride
            : plane.data;
        return {begin, plane.size};
    }
    const MediaPlaneView* Plane(std::size_t index) const noexcept {
        return index < planes_.size() ? &planes_[index] : nullptr;
    }
    const std::vector<MediaPlaneView>& Planes() const noexcept { return planes_; }
    // The owner may alias a backend object while retaining its resource wrapper.
    const std::shared_ptr<const void>& Owner() const noexcept { return owner_; }
    std::size_t PlaneSize(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].size : 0;
    }
    PlaneGeometry Geometry(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].geometry : PlaneGeometry{};
    }
    std::size_t TotalSize() const noexcept {
        std::size_t total = 0;
        for (const auto& plane : planes_) total += plane.size;
        return total;
    }
    bool IsContiguous() const noexcept {
        return contiguous_region_.data != nullptr ||
            (planes_.size() == 1 && planes_[0].data != nullptr);
    }
    std::optional<std::span<const uint8_t>> ContiguousBytes() const noexcept {
        if (contiguous_region_.data) return contiguous_region_.Bytes();
        if (planes_.size() == 1) return Data(0);
        return std::nullopt;
    }
    std::vector<ConstBufferRegion> MemoryRegions() const {
        if (contiguous_region_.data) return {contiguous_region_};
        std::vector<ConstBufferRegion> regions;
        regions.reserve(planes_.size());
        for (std::size_t i = 0; i < planes_.size(); ++i) {
            const auto bytes = Data(i);
            regions.push_back({bytes.data(), bytes.size()});
        }
        return regions;
    }

private:    
    std::vector<MediaPlaneView> planes_;
    /// @brief 连续内存区域
    ConstBufferRegion contiguous_region_;
    std::shared_ptr<const void> owner_;
};

inline std::shared_ptr<MediaBuffer> MediaBuffer::CopyContiguous(const MediaBufferView& source) {
    auto result = std::make_shared<MediaBuffer>();
    result->has_contiguous_storage_ = true;
    std::size_t total = 0;
    for (const auto& plane : source.Planes()) {
        if (!plane.IsValid() || plane.geometry.row_bytes >
                std::numeric_limits<std::size_t>::max() / plane.geometry.rows ||
            plane.geometry.row_bytes * plane.geometry.rows >
                std::numeric_limits<std::size_t>::max() - total) {
            return {};
        }
        total += plane.geometry.row_bytes * plane.geometry.rows;
    }
    result->contiguous_storage_.resize(total);
    std::size_t offset = 0;
    for (const auto& plane : source.Planes()) {
        const auto size = plane.geometry.row_bytes * plane.geometry.rows;
        for (std::size_t row = 0; row < plane.geometry.rows; ++row) {
            std::memcpy(result->contiguous_storage_.data() + offset + row * plane.geometry.row_bytes,
                plane.data + static_cast<std::ptrdiff_t>(row) * plane.geometry.stride,
                plane.geometry.row_bytes);
        }
        result->planes_.emplace_back(result->contiguous_storage_.data() + offset, size,
            PlaneGeometry{static_cast<std::ptrdiff_t>(plane.geometry.row_bytes),
                plane.geometry.row_bytes, plane.geometry.rows});
        offset += size;
    }
    return result;
}

inline std::shared_ptr<MediaBuffer> MediaBuffer::CopySeparatePlanes(const MediaBufferView& source) {
    auto result = std::make_shared<MediaBuffer>();
    for (const auto& plane : source.Planes()) {
        if (!plane.IsValid() || plane.geometry.row_bytes >
                std::numeric_limits<std::size_t>::max() / plane.geometry.rows) {
            return {};
        }
        const auto size = plane.geometry.row_bytes * plane.geometry.rows;
        std::vector<uint8_t> packed(size);
        for (std::size_t row = 0; row < plane.geometry.rows; ++row) {
            std::memcpy(packed.data() + row * plane.geometry.row_bytes,
                plane.data + static_cast<std::ptrdiff_t>(row) * plane.geometry.stride,
                plane.geometry.row_bytes);
        }
        result->PushPlane(packed.data(), packed.size(),
            PlaneGeometry{static_cast<std::ptrdiff_t>(plane.geometry.row_bytes),
                plane.geometry.row_bytes, plane.geometry.rows});
    }
    return result;
}
