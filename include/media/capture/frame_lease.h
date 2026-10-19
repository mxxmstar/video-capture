#pragma once

#include <memory>
#include "media/i_media_buffer.h"

class CaptureQueue;
class MmapBufferPool;

/// @brief 计数引用计数的帧租约类，不持有帧数据的所有权
class FrameLease final : public IMediaBuffer {
public:
    ~FrameLease() override;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    /// @brief 获取帧数据指针
    uint8_t* Data() override { return data_; }
    /// @brief 获取帧数据指针
    const uint8_t* Data() const override { return data_; }
    /// @brief 获取帧数据大小
    std::size_t Size() const override { return size_; }
    
    /// @brief 获取帧代际号
    std::uint64_t Generation() const { return generation_; }
    /// @brief 获取帧索引    
    unsigned Index() const { return index_; }
private:
    friend class CaptureQueue;
    FrameLease(std::shared_ptr<CaptureQueue> queue, std::shared_ptr<MmapBufferPool> storage,
               unsigned index, std::uint64_t generation, uint8_t* data, std::size_t size);
    std::shared_ptr<CaptureQueue> queue_;
    std::shared_ptr<MmapBufferPool> storage_;
    unsigned index_;
    std::uint64_t generation_;
    uint8_t* data_;
    std::size_t size_;
};
