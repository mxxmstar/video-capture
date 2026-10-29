#include "media/capture/v4l2_video_capture.h"
#include "media/capture/frame_lease.h"
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Open(V4L2VideoCapture& capture) {
    auto result = capture.Open();
    if (result.error) throw std::runtime_error(result.error->message);
}
std::shared_ptr<MediaFrame> Read(V4L2VideoCapture& capture) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = capture.ReadFrame();
        if (result.status == CaptureReadStatus::Frame) return result.frame;
        if (result.status != CaptureReadStatus::NoData)
            throw std::runtime_error(result.error ? result.error->message : "Unexpected stop");
    }
    throw std::runtime_error("Frame deadline exceeded");
}
std::vector<uint8_t> Bytes(const MediaFrame& frame) {
    std::vector<uint8_t> bytes;
    for (int p = 0; p < frame.PlaneCount(); ++p) {
        const auto plane = frame.PlaneView(p);
        Require(plane.data != nullptr, "Invalid leased plane view");
        for (int row = 0; row < plane.geometry.rows; ++row) {
            const auto* data = plane.data + static_cast<std::ptrdiff_t>(row) * plane.geometry.stride;
            bytes.insert(bytes.end(), data, data + plane.geometry.row_bytes);
        }
    }
    return bytes;
}
std::size_t DescriptorCount() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) ++count;
    return count;
}
std::size_t MappingCount(const std::string& device) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::size_t count = 0;
    while (std::getline(maps, line)) if (line.find(device) != std::string::npos) ++count;
    return count;
}
}

int main(int argc, char** argv) {
    try {
        // ========== 用例参数解析 ==========
        // ./v4l2_lease_test "/dev/video0" "yuyv"
        if (argc != 3) throw std::runtime_error("Usage: v4l2_lease_test DEVICE yuyv|nv12|i420");
        V4L2CaptureConfig config;
        config.device = argv[1];
        const std::string format = argv[2];
        if (format == "nv12") config.pixel_format = PixelFormat::kNV12;
        else if (format == "i420") config.pixel_format = PixelFormat::kI420;
        else Require(format == "yuyv", "Unknown format");
        config.buffer_count = 2;                             // 故意只申请 2 个 buffer，方便耗尽测试
        config.poll_timeout = std::chrono::seconds(3);

        // 记录测试前的 fd 数量和 mmap 映射数量，用于最后的资源泄漏检查
        const auto descriptors_before = DescriptorCount();
        const auto mappings_before = MappingCount(config.device);

        // ========== 测试 1: 多消费者共享引用（引用计数不会过早回收） ==========
        V4L2VideoCapture capture(config);
        Open(capture);

        // 把所有 buffer 都取出（此时队列耗尽，HasQueued() == false）
        std::vector<std::shared_ptr<MediaFrame>> held;
        for (unsigned i = 0; i < capture.GetVideoInfo().buffer_count; ++i)
            held.push_back(Read(capture));

        // 验证 MmapLease 模式下返回的是 FrameLease（零拷贝引用 mmap 内存）
        Require(held.front()->view.PlaneCount() > 0,
                "MmapLease mode did not return a view-only frame");
        const auto* retained_mapping = held.front()->PlaneView(0).data;

        const auto saved = Bytes(*held.front());              // 保存第一帧的校验数据

        // 构造两个消费者引用同一个 buffer（后一帧的 buffer，会被 moved out）
        auto second_consumer = held.back()->view;           // 消费者 A：保留引用
        auto released = std::move(held.back());               // 消费者 B：取出独占所有权
        held.pop_back();

        // 启动采集线程，尝试获取新帧——因为队列耗尽 + 没有 buffer 归还，会阻塞在 poll
        auto pending = std::async(std::launch::async, [&] { return Read(capture); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 释放消费者 B：此时 second_consumer 还活着，slot 不应被回收，pending 继续阻塞
        auto release_first = std::async(std::launch::async,
            [frame = std::move(released)]() mutable { frame.reset(); });
        release_first.get();
        Require(pending.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
                "Buffer recycled before last consumer release");

        // 释放消费者 A：最后一个消费者释放后，slot → RecyclePending → DrainReturns → QBUF → 采集线程被唤醒
        auto release_last = std::async(std::launch::async,
            [view = std::move(second_consumer)]() mutable { view = {}; });
        release_last.get();
        Require(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "Cross-thread release did not wake exhausted capture queue");
        held.push_back(pending.get());

        // 重新取回的 buffer 数据没有被覆盖（被多消费者保护期间，slot 未归还，驱动无法写入）
        Require(Bytes(*held.front()) == saved, "Leased contents overwritten");

        // ========== 测试 2: 队列耗尽时 Stop 能中断阻塞的 ReadFrame ==========
        // 所有 buffer 都被持有 → ReadFrame 阻塞在 poll（等归还或停止信号）
        auto stopped = std::async(std::launch::async, [&] { return capture.ReadFrame(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Require(stopped.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "Stop test did not block");

        // RequestStop → write(wake_fd_) → poll 被唤醒 → 检测到停止信号 → 返回 Stopped
        capture.RequestStop();
        Require(stopped.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready,
                "Stop failed to interrupt exhausted queue");
        Require(stopped.get().status == CaptureReadStatus::Stopped, "Expected Stopped");

        // Close 后，held 中的帧数据不应被破坏（mmap 由 MmapBufferPool + FrameLease 的 shared_ptr 保护）
        capture.Close();
        Require(Bytes(*held.front()) == saved, "Close invalidated retained mapping");

        // ========== 测试 3: 重复 open/close 的 generation 递增和旧代映射保护 ==========
        for (int i = 0; i < 5; ++i) {
            Open(capture);
            auto current = Read(capture);
            Require(current->PlaneView(0).data != nullptr, "Reopened capture returned no plane");
            if (!held.empty()) Require(current->PlaneView(0).data != retained_mapping,
                                      "Reopen reused a retained mapping");
            if (!held.empty()) {
                // 旧代 held 的映射在 reopen 后依然有效，内容不受新采集影响
                Require(Bytes(*held.front()) == saved, "Reopen overwrote old generation");
                auto release_old = std::async(std::launch::async,
                    [old = std::move(held)]() mutable { old.clear(); });
                release_old.get();
                held.clear();
            }
            Read(capture);                                    // 再取一帧，确保队列正常运行
            capture.Close();
            capture.Close();                                  // 验证重复 Close 是幂等的
        }

        // ========== 测试 4: FrameLease 生命周期大于 Capture 对象 ==========
        // 模拟场景：采集对象已销毁，但外部还持有未处理的帧
        std::shared_ptr<MediaFrame> survivor;
        {
            V4L2VideoCapture temporary(config);
            Open(temporary);
            survivor = Read(temporary);
        } // temporary 在此析构 → Retire → REQBUFS(0) → munmap
        // survivor 仍然有效（MmapBufferPool 的 shared_ptr 引用未释放）
        const auto survivor_bytes = Bytes(*survivor);

        // ========== 测试 5: Copy 交付模式 vs MmapLease 模式对比 ==========
        config.delivery_mode = V4L2DeliveryMode::Copy;
        V4L2VideoCapture copying(config);
        Open(copying);
        std::vector<std::shared_ptr<MediaFrame>> copies;
        for (unsigned i = 0; i < copying.GetVideoInfo().buffer_count + 3; ++i)
            copies.push_back(Read(copying));
        // Copy 模式下 buffer 不是 FrameLease，而是独立的内存拷贝
        Require(copies.front()->view.PlaneCount() > 0 &&
                copies.front()->Stride(0) == copies.front()->PlaneView(0).geometry.row_bytes,
                "Copy mode did not return compact plane views");
        const auto copy_bytes = Bytes(*copies.front());
        copying.Close();
        // Copy 模式的数据在 Close 后仍有效（已从驱动内存中深拷贝出来）
        Require(Bytes(*copies.front()) == copy_bytes, "Copy changed after Close");
        // MmapLease 模式的数据在 Capture 对象销毁后仍有效
        Require(Bytes(*survivor) == survivor_bytes, "Capture destruction invalidated lease");

        // ========== 清理与资源泄漏检查 ==========
        survivor.reset();
        copies.clear();
        held.clear();
        // 验证所有 fd 已释放
        Require(DescriptorCount() == descriptors_before, "Capture or queue descriptor leaked");
        // 验证所有 mmap 已释放
        Require(MappingCount(config.device) == mappings_before, "Retired generation mapping leaked");
        std::cout << format << ": lease exhaustion, consumers, stop, reopen, destruction and copy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}