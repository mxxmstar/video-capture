#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// @brief V4L2 VIDIOIOC_QBUF 缓冲区映射池的 RAII 类
///
/// @details 该类负责将文件描述符的缓冲区映射到进程地址空间。
class MmapBufferPool final {
public:
    struct Mapping { void* address; std::size_t length; };
    ~MmapBufferPool();
    MmapBufferPool() = default;
    MmapBufferPool(const MmapBufferPool&) = delete;
    MmapBufferPool& operator=(const MmapBufferPool&) = delete;

    /// @brief 映射文件描述符的缓冲区
    /// @param fd 文件描述符
    /// @param length 缓冲区长度
    /// @param offset 偏移量
    /// @return 0 成功，其他值 失败
    int Map(int fd, std::size_t length, std::uint32_t offset);
    
    /// @brief 获取映射的缓冲区
    /// @param index 缓冲区索引
    /// @return 缓冲区映射
    /// @throw std::out_of_range 索引超出范围
    /// @throw std::bad_alloc 内存分配失败
    /// @throw std::runtime_error 其他运行时错误
    const Mapping& At(unsigned index) const { return mappings_.at(index); }
private:
    std::vector<Mapping> mappings_;
};
