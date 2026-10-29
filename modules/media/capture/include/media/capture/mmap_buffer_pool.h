#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>


struct MediaBufferView;
struct MediaPlaneView;

/// @brief V4L2 VIDIOIOC_QBUF 缓冲区映射池的 RAII 类
///
/// @details 该类负责将文件描述符的缓冲区映射到进程地址空间。
class MmapBufferPool final {
public:
    struct Mapping {
        void* address;    ///< 当前平面的虚拟内存首地址
        std::size_t length; ///< 当前平面的长度
        std::uint32_t offset; ///< 当前平面的偏移量
    };

    using FramePlanes = std::vector<Mapping>;
    ~MmapBufferPool();
    MmapBufferPool() = default;
    MmapBufferPool(const MmapBufferPool&) = delete;
    MmapBufferPool& operator=(const MmapBufferPool&) = delete;

    
    /// @brief 重新调整缓冲区映射池的槽位数量
    /// @param count 新的槽位数量
    /// @throw std::logic_error 缓冲区映射池已初始化
    void ResizeSlots(std::size_t count);

    /// @brief 映射特定槽位(index)下的某个平面
    /// @param fd 文件描述符
    /// @param index 视频帧的缓存索引 (Buffer Index)
    /// @param length 该平面的长度
    /// @param offset 该平面的偏移量
    /// @return 0 成功，其他值 失败
    int Map(int fd, unsigned index, std::size_t length, std::uint32_t offset);
    
    /// @brief 获取指定视频帧槽位的所有平面映射
    /// @param index 视频帧的缓存索引 (Buffer Index)
    /// @return 该帧对应的所有平面映射集合
    /// @throw std::out_of_range 索引超出范围
    const FramePlanes& At(unsigned index) const;

    MediaBufferView View(unsigned index) const;
private:
    std::vector<FramePlanes> pool_;
};