#pragma once
/// @file simple_buffer_view.h
/// 拥有式 IMediaBuffer 视图实现。
///
/// 通过 shared_ptr<const uint8_t> 共享源数据所有权；
/// Data() 返回 shared_ptr::get()，类型系统保证视图存活期间数据有效。
///
/// 与 std::string_view + 引用计数保活 等价：不复制数据，但确保持有一定所有权。
///
/// 用于 CreateCompact 的零拷贝快速路径：当源布局已满足紧凑格式时，
/// 跳过 memcpy，将 shared_ptr 直接包装为 SimpleBufferView。

#include <cstdint>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include "media/i_media_buffer.h"

class SimpleBufferView : public IMediaBuffer {
public:
    /// @brief 延长源数据的生命周期，且允许指定是否是只读权限
    /// @param buffer  拥有源数据的 shared_ptr；Data() 返回其 get()
    /// @param size    有效数据字节数
    /// @param is_read_only 是否只读权限
    SimpleBufferView(std::shared_ptr<const uint8_t> buffer, std::size_t size, bool is_read_only = false)
        : buffer_(std::move(buffer)), size_(size), is_read_only_(is_read_only) {}

    /// @brief 包装纯外部指针，不进行任何保活。
    /// @warning 调用方必须严格保证在整个 SimpleBufferView 存活期间，data 指向的内存持续有效！
    SimpleBufferView(const void* data, std::size_t size)
        : buffer_(std::shared_ptr<const uint8_t>(
              std::shared_ptr<const uint8_t>(), // 一个空的控制块，不持有任何对象
              static_cast<const uint8_t*>(data)  // 仅仅存储这个裸指针的值
          )), 
          size_(size), 
          is_read_only_(false) {
    }

    uint8_t* Data() override {
        if (is_read_only_) {
            throw std::runtime_error("SimpleBufferView is read-only");
        }
        return const_cast<uint8_t*>(buffer_.get());
    }
    
    const uint8_t* Data() const override { return buffer_.get(); }
    std::size_t Size() const override { return size_; }

private:
    std::shared_ptr<const uint8_t> buffer_;
    std::size_t size_;
    bool is_read_only_;
};