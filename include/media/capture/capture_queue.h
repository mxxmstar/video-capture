#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <linux/videodev2.h>
#include "media/capture/frame_lease.h"
#include "media/capture/mmap_buffer_pool.h"

/// @brief 封装V4L2缓冲区的生命周期管理类
class CaptureQueue final : public std::enable_shared_from_this<CaptureQueue> {
public:
    /// @brief 缓冲区状态枚举
    enum class SlotState {
        Queued, ///< 通过 VIDIOC_QBUF 入队到内核
        Leased, ///< 已被 Dequeue 出队, 被消费者 FrameLease 持有
        RecyclePending, ///< 消费者 FrameLease 已被释放, 等待 DrainReturns 重新入队
        Retired ///< 已被 Retire 释放, 不再被使用
    };

    /// @brief 构造函数
    /// @param borrowed_fd 已打开的V4L2设备描述符    
    explicit CaptureQueue(int borrowed_fd);
    ~CaptureQueue();
    CaptureQueue(const CaptureQueue&) = delete;
    CaptureQueue& operator=(const CaptureQueue&) = delete;

    /// @brief 请求 count 个缓冲区，每个缓冲区大小为 minimum_size 字节，全部 VIDIOC_QBUF 入队
    /// @param count 缓冲区数量
    /// @param minimum_size 缓冲区最小大小
    /// @return 0 成功，其他值 失败
    int Initialize(unsigned count, unsigned minimum_size);

    /// @brief 从内核中出队一个已经填充视频数据的缓冲区
    /// @param buffer 缓冲区结构体
    /// @param lease 缓冲区租约指针
    /// @return 0 成功，其他值 失败
    int Dequeue(v4l2_buffer& buffer, std::shared_ptr<FrameLease>& lease);

    /// @brief 遍历缓冲区，将所有 RecyclePending 状态的缓冲区重新入队到内核
    /// @return 0 成功，其他值 失败
    int DrainReturns();

    /// @brief 释放所有缓冲区，将所有状态设置为 Retired
    /// @return 0 成功，其他值 失败
    int Retire() noexcept;

    /// @brief 获取用于唤醒消费者线程的事件文件描述符
    int WakeFd() const { return wake_fd_; }
    
    /// @brief 缓冲区数量
    unsigned Count() const { return static_cast<unsigned>(slots_.size()); }
    /// @brief 当前队列的代际号
    std::uint64_t Generation() const { return generation_; }
    /// @brief 是否有正在 QBUF 入队的缓冲区
    bool HasQueued() const;
private:
    friend class FrameLease;

    /// @brief 释放缓冲区，FrameLease 析构时调用，通知 DrainReturns 线程重新入队该缓冲区
    void release(unsigned index, std::uint64_t generation) noexcept;
    int fd_;    ///< 已打开的V4L2设备描述符
    int wake_fd_{-1};    ///< 用于唤醒消费者线程的事件文件描述符
    const std::uint64_t generation_;    ///< 当前 capture_queue 的代际号
    mutable std::mutex mutex_;  ///< 缓冲区状态操作互斥锁
    bool active_{true}; ///< 是否运行中
    int error_{0};  ///< 最近一次操作的错误码
    std::vector<SlotState> slots_;  ///< 保存每个缓冲区的状态
    std::shared_ptr<MmapBufferPool> storage_;   ///< 缓冲区存储池
};
