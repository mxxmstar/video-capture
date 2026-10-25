#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

struct PlaneGeometry {
    std::ptrdiff_t stride = 0;
    std::size_t row_bytes = 0;
    std::size_t rows = 0;
};

struct MediaPlane {
    std::vector<uint8_t> data;
    PlaneGeometry geometry;

    MediaPlane(const uint8_t* src, std::size_t size, PlaneGeometry g)
        : data(src, src + size), geometry(g) {
        assert(src != nullptr || size == 0);
    }
    MediaPlane(const uint8_t* src, std::size_t size,
               std::ptrdiff_t stride, std::size_t rows)
        : MediaPlane(src, size, PlaneGeometry{stride, stride >= 0 ? static_cast<std::size_t>(stride) : 0, rows}) {}
};

struct MediaPlaneView {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    PlaneGeometry geometry;

    MediaPlaneView() = default;
    MediaPlaneView(const uint8_t* src, std::size_t src_size, PlaneGeometry g)
        : data(src), size(src_size), geometry(g) {}
    MediaPlaneView(const MediaPlane& plane)
        : data(plane.data.data()), size(plane.data.size()), geometry(plane.geometry) {}
};

class MediaBuffer {
public:
    MediaBuffer() = default;
    MediaBuffer(const MediaBuffer&) = delete;
    MediaBuffer& operator=(const MediaBuffer&) = delete;
    MediaBuffer(MediaBuffer&&) noexcept = default;
    MediaBuffer& operator=(MediaBuffer&&) noexcept = default;

    void Push(const uint8_t* data, std::size_t size,
              std::ptrdiff_t stride, std::size_t rows) {
        planes_.emplace_back(data, size, stride, rows);
    }
    void Push(const uint8_t* data, std::size_t size, PlaneGeometry geometry) {
        planes_.emplace_back(data, size, geometry);
    }
    std::size_t PlaneCount() const noexcept { return planes_.size(); }
    uint8_t* Data(std::size_t index) noexcept {
        return index < planes_.size() ? planes_[index].data.data() : nullptr;
    }
    const uint8_t* Data(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].data.data() : nullptr;
    }
    std::size_t PlaneSize(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].data.size() : 0;
    }
    PlaneGeometry Geometry(std::size_t index) const noexcept {
        return index < planes_.size() ? planes_[index].geometry : PlaneGeometry{};
    }
    std::size_t TotalSize() const noexcept {
        std::size_t total = 0;
        for (const auto& plane : planes_) total += plane.data.size();
        return total;
    }
    bool IsContiguous() const noexcept { return planes_.size() == 1; }

private:
    friend class MediaBufferView;
    std::vector<MediaPlane> planes_;
};

class MediaBufferView {
public:
    MediaBufferView() = default;
    explicit MediaBufferView(std::vector<MediaPlaneView> planes)
        : planes_(std::move(planes)) {}
    MediaBufferView(std::vector<MediaPlaneView> planes,
                    std::shared_ptr<const void> owner)
        : planes_(std::move(planes)), owner_(std::move(owner)) {}

    static MediaBufferView FromOwner(std::shared_ptr<const MediaBuffer> owner) {
        std::vector<MediaPlaneView> planes;
        if (owner) {
            planes.reserve(owner->planes_.size());
            for (const auto& plane : owner->planes_) planes.emplace_back(plane);
        }
        return {std::move(planes), std::move(owner)};
    }

    std::size_t PlaneCount() const noexcept { return planes_.size(); }
    std::span<const uint8_t> Data(std::size_t index) const noexcept {
        if (index >= planes_.size()) return {};
        return {planes_[index].data, planes_[index].size};
    }
    const MediaPlaneView* Plane(std::size_t index) const noexcept {
        return index < planes_.size() ? &planes_[index] : nullptr;
    }
    const std::vector<MediaPlaneView>& Planes() const noexcept { return planes_; }
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
    bool IsContiguous() const noexcept { return planes_.size() == 1; }

private:
    std::vector<MediaPlaneView> planes_;
    std::shared_ptr<const void> owner_;
};
