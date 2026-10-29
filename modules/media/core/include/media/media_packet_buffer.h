#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>


class MediaPacketBuffer final {
public:
    MediaPacketBuffer() = default;

    /// @brief 从字节数组创建MediaPacketBuffer
    /// @param bytes 字节数组
    explicit MediaPacketBuffer(std::vector<uint8_t> bytes) {
        auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        data_ = storage->data();
        size_ = storage->size();
        owner_ = std::move(storage);
    }

    /// @brief 从数据指针和大小拷贝创建MediaPacketBuffer
    /// @param data 数据指针
    /// @param size 数据大小
    /// @throw std::invalid_argument 如果数据指针为空且大小不为0
    MediaPacketBuffer(const void* data, std::size_t size) {
        if (!data && size != 0) throw std::invalid_argument("Null packet payload");
        std::vector<uint8_t> bytes;
        if (size) {
            const auto* begin = static_cast<const uint8_t*>(data);
            bytes.assign(begin, begin + size);
        }
        *this = MediaPacketBuffer(std::move(bytes));
    }

    MediaPacketBuffer(const MediaPacketBuffer&) = default;
    MediaPacketBuffer& operator=(const MediaPacketBuffer&) = default;
    MediaPacketBuffer(MediaPacketBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
          owner_(std::move(other.owner_)) {}

    /// @brief 移动赋值运算符
    /// @param other 要移动的MediaPacketBuffer
    /// @return 本MediaPacketBuffer的引用
    MediaPacketBuffer& operator=(MediaPacketBuffer&& other) noexcept {
        if (this != &other) {
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            owner_ = std::move(other.owner_);
        }
        return *this;
    }
    
    /// @brief 包装外部存储的MediaPacketBuffer，数据所有者必须保留数据的生命周期
    /// @param data 数据指针
    /// @param size 数据大小
    /// @param owner 数据所有者
    /// @return 包装后的MediaPacketBuffer
    static std::shared_ptr<MediaPacketBuffer> Wrap(const uint8_t* data, std::size_t size, std::shared_ptr<const void> owner) {
        if (size != 0 && (!data || owner.use_count() == 0)) {
            throw std::invalid_argument("Packet payload requires data and an owner");
        }
        auto result = std::make_shared<MediaPacketBuffer>();
        result->data_ = data;
        result->size_ = size;
        result->owner_ = std::move(owner);
        return result;
    }

    const uint8_t* Data() const noexcept { return data_; }
    std::size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }
    std::span<const uint8_t> Bytes() const noexcept { return {data_, size_}; }
    const std::shared_ptr<const void>& Owner() const noexcept { return owner_; }

private:
    const uint8_t* data_ = nullptr; ///< 数据指针
    std::size_t size_ = 0;  ///< 数据大小
    std::shared_ptr<const void> owner_; ///< 数据所有者
};
