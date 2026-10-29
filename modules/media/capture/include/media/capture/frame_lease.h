#pragma once

#include <memory>
#include "media/media_buffer.h"

class CaptureQueue;
class MmapBufferPool;

class FrameLease : public std::enable_shared_from_this<FrameLease> {
public:
    ~FrameLease();
    /// @brief 获取帧代际号
    std::uint64_t Generation() const { return generation_; }

    /// @brief 获取帧索引    
    unsigned Index() const { return index_; }
    /// @brief 获取帧视图
    MediaBufferView View() const { return MediaBufferView(view_.Planes(), shared_from_this()); }
private:
    friend class CaptureQueue;  // 仅允许 CaptureQueue 创建 FrameLease
    /// @brief 构造函数
    /// @param queue 捕获队列
    /// @param storage 缓冲区映射池
    /// @param index 视频帧的缓存索引 (Buffer Index)
    /// @param generation 帧代际号
    /// @param planeviews 视频帧的平面视图
    /// @throw std::logic_error 缓冲区映射池未初始化
    /// @throw std::out_of_range 索引超出范围
    FrameLease(std::shared_ptr<CaptureQueue> queue, std::shared_ptr<MmapBufferPool> storage,
        unsigned index, std::uint64_t generation, std::vector<MediaPlaneView> planeviews);
    
    
    std::shared_ptr<CaptureQueue> queue_;   ///< 捕获队列
    std::shared_ptr<MmapBufferPool> storage_; ///< 缓冲区映射池
    unsigned index_;     ///< 视频帧的缓存索引 (Buffer Index)
    std::uint64_t generation_; ///< 帧代际号
    MediaBufferView view_; ///< 视频帧的视图
};
