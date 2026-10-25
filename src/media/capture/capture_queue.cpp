#include "common/log/logger.h"
#include "media/capture/capture_queue.h"
#include <atomic>
#include <cerrno>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <array>
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
}
CaptureQueue::CaptureQueue(int borrowed_fd, unsigned buffer_type)
    : fd_(borrowed_fd), buffer_type_(buffer_type), generation_(next_generation.fetch_add(1)),
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

int CaptureQueue::Initialize(unsigned count, unsigned minimum_size, std::vector<PlaneGeometry> plane_geometry) {
    plane_geometry_ = std::move(plane_geometry);
    // 向内核查询
    v4l2_requestbuffers request{};
    request.type = buffer_type_;
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
    storage_->ResizeSlots(request.count);

    const bool is_mplane = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    for (unsigned index = 0; index < request.count; ++index) {
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        v4l2_buffer buffer{};
        buffer.type = buffer_type_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (is_mplane) {
            buffer.length = VIDEO_MAX_PLANES;
            buffer.m.planes = planes.data();
        }
        if (Ioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            LOG_ERROR("Failed to query buffer {} from kernel: {}", index, errno);
            return errno;
        }

        if (is_mplane) {
            std::size_t total_length = 0;
            for (unsigned plane_index = 0; plane_index < buffer.length; ++plane_index) {
                total_length += planes[plane_index].length;

                const int error = storage_->Map(fd_, index, planes[plane_index].length, planes[plane_index].m.mem_offset);
                if (error) {
                    LOG_ERROR("Failed to map plane {} of buffer {} to memory: {}", plane_index, index, error);
                    return error;
                }
            }
            if (total_length < minimum_size) {
                LOG_ERROR("Buffer-{} total length {} is too small, minimum size is {}", index, total_length, minimum_size);
                return EPROTO;
            }
        } else {
            if (buffer.length < minimum_size) {
                LOG_ERROR("Buffer-{} length {} is too small, minimum size is {}", index, buffer.length, minimum_size);
                return EPROTO;
            }
            const int error = storage_->Map(fd_, index, buffer.length, buffer.m.offset);
            if (error) {
                LOG_ERROR("Failed to map buffer {} to memory: {}", index, error);
                return error;
            }
        }

        // 将 buffer 入队到内核队列
        if (Ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("Failed to queue buffer {} to kernel: {}", index, errno);
            return errno;
        }
        slots_[index] = SlotState::Queued;
    }
    return 0;
}

int CaptureQueue::Dequeue(v4l2_buffer& buffer, std::shared_ptr<FrameLease>& lease) {
    const bool is_mplane = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buffer.type   = buffer_type_;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (is_mplane) {
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes.data();
    }
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
    
    // 检查 bytesused 是否超过映射内存长度
    const auto& frame_mapping = storage_->At(buffer.index);
    std::size_t buf_bytesused = 0;
    std::vector<MediaPlaneView> views;
    if (is_mplane) {
        for (unsigned plane_index = 0; plane_index < buffer.length; ++plane_index) {
            if (planes[plane_index].bytesused > frame_mapping[plane_index].length) {
                LOG_ERROR("Plane {} bytesused {} exceeds mapped length {}", plane_index, planes[plane_index].bytesused, frame_mapping[plane_index].length);
                slots_[buffer.index] = SlotState::RecyclePending;
                return EPROTO;
            }
            const auto& geo = plane_geometry_[plane_index];
            views.push_back(MediaPlaneView{
                static_cast<uint8_t*>(frame_mapping[plane_index].address),
                planes[plane_index].bytesused,
                geo});
            buf_bytesused += planes[plane_index].bytesused;
        }
    } else {
        buf_bytesused = buffer.bytesused;
        if (buf_bytesused > frame_mapping[0].length) {
            LOG_ERROR("Kernel bytesused {} exceeds mapped memory length {}", buf_bytesused, frame_mapping[0].length);
            slots_[buffer.index] = SlotState::RecyclePending;
            return EPROTO;
        }
        const auto& geo = plane_geometry_[0];
        views.push_back(MediaPlaneView{
            static_cast<uint8_t*>(frame_mapping[0].address),
            buf_bytesused,
            geo});
    }
    
    // 构造 FrameLease
    try {
        auto* raw = new FrameLease(shared_from_this(), storage_, buffer.index, generation_, std::move(views));
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
    const bool is_mplane = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    for (unsigned index = 0; index < slots_.size(); ++index) {
        if (slots_[index] != SlotState::RecyclePending) {
            continue;
        }
        
        const auto& frame_planes = storage_->At(index);
        v4l2_buffer buffer{};
        buffer.type = buffer_type_;
        buffer.memory   = V4L2_MEMORY_MMAP;
        buffer.index    = index;
        if (is_mplane) {
            uint32_t plane_count = static_cast<uint32_t>(frame_planes.size());
            buffer.length = plane_count;
            std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
            for (unsigned plane_index = 0; plane_index < plane_count; ++plane_index) {
                planes[plane_index].length = static_cast<__u32>(frame_planes[plane_index].length);
                planes[plane_index].m.mem_offset = frame_planes[plane_index].offset;
            }
            buffer.m.planes = planes.data();
        } else {
            buffer.length = static_cast<__u32>(frame_planes[0].length);
        }
        
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
    request.type = buffer_type_;
    request.memory = V4L2_MEMORY_MMAP;
    const int error = Ioctl(fd_, VIDIOC_REQBUFS, &request) < 0 ? errno : 0;
    fd_ = -1;
    return error;
}