#include "common/log/logger.h"
#include "media/capture/capture_queue.h"
#include <atomic>
#include <cerrno>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
std::atomic<std::uint64_t> next_generation{1};
/// @brief 调用 ioctl 函数，处理 EINTR 错误
/// @param fd 文件描述符
/// @param command ioctl 命令
/// @param argument ioctl 参数
/// @return ioctl 返回值
int Ioctl(int fd, unsigned long command, void* argument) {
    int result;
    do { 
        result = ::ioctl(fd, command, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}
v4l2_buffer Buffer(unsigned index = 0) {
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    return buffer;
}
}

CaptureQueue::CaptureQueue(int borrowed_fd) : fd_(borrowed_fd), generation_(next_generation.fetch_add(1)),
    storage_(std::make_shared<MmapBufferPool>()) {
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);    // 非阻塞，关闭时自动关闭
    if (wake_fd_ < 0) {
        LOG_ERROR("Failed to create eventfd: {}", errno);
        throw std::system_error(errno, std::generic_category(), "queue eventfd");
    }
}

CaptureQueue::~CaptureQueue() {    
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
    }
}

int CaptureQueue::Initialize(unsigned count, unsigned minimum_size) {
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = count;
    if (Ioctl(fd_, VIDIOC_REQBUFS, &request) < 0) {
        LOG_ERROR("Failed to request buffers: {}", errno);
        return errno;
    }
    if (request.count < 2 || request.count > 64) {
        LOG_ERROR("Invalid request buffer count from kernel: {}", request.count);
        return EPROTO;
    }
    LOG_INFO("Initialized queue with {} buffers", request.count);
    slots_.assign(request.count, SlotState::Retired);

    for (unsigned index = 0; index < request.count; ++index) {
        auto buffer = Buffer(index);
        if (Ioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            LOG_ERROR("Failed to query buffer {} from kernel: {}", index, errno);
            return errno;
        }
        if (buffer.length < minimum_size) {
            LOG_ERROR("Buffer-{} length {} is too small, minimum size is {}", index, buffer.length, minimum_size);
            return EPROTO;
        }
        if (const int error = storage_->Map(fd_, buffer.length, buffer.m.offset)) {
            LOG_ERROR("Failed to map buffer {} to memory: {}", index, error);
            return error;
        }
        if (Ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("Failed to queue buffer {} to kernel: {}", index, errno);
            return errno;
        }
        slots_[index] = SlotState::Queued;
    }
    return 0;
}

int CaptureQueue::Dequeue(v4l2_buffer& buffer, std::shared_ptr<FrameLease>& lease) {
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        LOG_ERROR("Failed to dequeue buffer from kernel: {}", errno);
        return errno;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!active_ || buffer.index >= slots_.size() || slots_[buffer.index] != SlotState::Queued) {
        LOG_ERROR("Invalid buffer index {} from kernel: {}", buffer.index, errno);
        return error_ = EPROTO;
    }        
    slots_[buffer.index] = SlotState::Leased;   // 标记为出队
    const auto& mapping = storage_->At(buffer.index);
    if (buffer.bytesused > mapping.length) {
        slots_[buffer.index] = SlotState::RecyclePending;   // 归还到队列
        return EPROTO;
    }
    // Allocate while the slot is leased; on failure the next read recycles it.
    try {
        auto* raw = new FrameLease(shared_from_this(), storage_, buffer.index, generation_, 
            static_cast<uint8_t*>(mapping.address), buffer.bytesused);
        lock.unlock();
        lease.reset(raw); // shared_ptr deletes raw (and signals Release) if allocation fails.
    } catch (...) {
        // 构造 FrameLease 失败，回滚标记为 归还到队列 状态
        if (lock.owns_lock()) {
            slots_[buffer.index] = SlotState::RecyclePending;
        }
        throw;
    }
    return 0;
}

void CaptureQueue::release(unsigned index, std::uint64_t generation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || generation != generation_ || index >= slots_.size() || slots_[index] != SlotState::Leased) {
        return;
    }
    slots_[index] = SlotState::RecyclePending;
    const std::uint64_t one = 1;
    ssize_t result;
    do {
        // 唤醒 DrainReturns 的线程
        result = ::write(wake_fd_, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) {
        error_ = errno;
    }
}

int CaptureQueue::DrainReturns() {
    std::uint64_t value;
    ssize_t result;
    do {
        result = ::read(wake_fd_, &value, sizeof(value));   // 有数可读说明有新的归还请求
    } while (result < 0 && errno == EINTR);
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_) {
        return error_;
    }
    if (!active_) {
        return ECANCELED;
    }
    // 遍历所有缓冲区，将被释放的缓冲区重新入队
    for (unsigned index = 0; index < slots_.size(); ++index) {
        if (slots_[index] != SlotState::RecyclePending) {
            continue;
        }
        auto buffer = Buffer(index);
        if (Ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            return error_ = errno;
        }
        slots_[index] = SlotState::Queued;
    }
    return 0;
}

bool CaptureQueue::HasQueued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto state : slots_) {
        if (state == SlotState::Queued) return true;
    }
    return false;
}

int CaptureQueue::Retire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return 0;
    }
    active_ = false;
    for (auto& state : slots_) {
        state = SlotState::Retired;
    }
    // 取消所有映射
    storage_.reset();
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    const int error = Ioctl(fd_, VIDIOC_REQBUFS, &request) < 0 ? errno : 0;
    fd_ = -1;
    return error;
}
