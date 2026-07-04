/**
 * @file test_spdlog.cpp
 * @brief spdlog 裁剪测试 demo
 * 
 * 测试功能：
 *   1. 同步日志 - 控制台输出
 *   2. 同步日志 - 文件输出（带轮转）
 *   3. 异步日志 - 控制台 + 文件输出
 *   4. 日志级别测试
 *   5. 格式化测试
 */

#include <iostream>
#include <thread>
#include <chrono>

// spdlog 头文件
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"

// 日志宏
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

void test_sync_console() {
    std::cout << "\n========== 测试 1: 同步日志 - 控制台 ==========" << std::endl;
    
    try {
        // 创建控制台 logger（带颜色）
        auto logger = spdlog::stdout_color_mt("console");
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%s:%#] %v");
        spdlog::set_default_logger(logger);
        
        // 设置日志级别
        spdlog::set_level(spdlog::level::trace);
        
        // 测试各级别日志
        LOG_TRACE("这是 TRACE 级别日志");
        LOG_DEBUG("这是 DEBUG 级别日志");
        LOG_INFO("这是 INFO 级别日志");
        LOG_WARN("这是 WARN 级别日志");
        LOG_ERROR("这是 ERROR 级别日志");
        LOG_CRITICAL("这是 CRITICAL 级别日志");
        
        // 测试格式化
        LOG_INFO("格式化测试: 整数={}, 浮点数={:.2f}, 字符串={}", 42, 3.14159, "hello");
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "日志初始化失败: " << ex.what() << std::endl;
    }
}

void test_sync_file() {
    std::cout << "\n========== 测试 2: 同步日志 - 文件输出 ==========" << std::endl;
    
    try {
        // 创建轮转文件 logger
        // 参数：文件名, 单文件最大大小(5MB), 最大文件数(3)
        auto logger = spdlog::rotating_logger_mt("file_logger", "logs/test_sync.log", 5 * 1024 * 1024, 3);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
        
        // 写入测试日志
        for (int i = 0; i < 10; ++i) {
            logger->info("同步文件日志 #{}", i);
        }
        
        // 刷新确保写入
        logger->flush();
        
        std::cout << "文件日志已写入: logs/test_sync.log" << std::endl;
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "日志初始化失败: " << ex.what() << std::endl;
    }
}

void test_async_multi_sink() {
    std::cout << "\n========== 测试 3: 异步日志 - 控制台 + 文件 ==========" << std::endl;
    
    try {
        // 初始化异步线程池
        spdlog::init_thread_pool(4096, 1);  // 队列大小 4096, 1 个后台线程
        
        // 创建多个 sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [async] %v");
        
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/test_async.log", 5 * 1024 * 1024, 3
        );
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [async] [%t] %v");
        
        // 创建异步 logger
        std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
        auto logger = std::make_shared<spdlog::async_logger>(
            "async_logger", sinks.begin(), sinks.end(), spdlog::thread_pool()
        );
        
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
        
        // 写入测试日志
        for (int i = 0; i < 20; ++i) {
            LOG_INFO("异步日志 #{}", i);
        }
        
        // 等待异步写入完成
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        logger->flush();
        
        std::cout << "异步日志已写入: logs/test_async.log" << std::endl;
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "日志初始化失败: " << ex.what() << std::endl;
    }
}

void test_level_filter() {
    std::cout << "\n========== 测试 4: 日志级别过滤 ==========" << std::endl;
    
    try {
        auto logger = spdlog::stdout_color_mt("level_test");
        logger->set_pattern("[%^%l%$] %v");
        spdlog::set_default_logger(logger);
        
        std::cout << "\n--- 设置为 DEBUG 级别 ---" << std::endl;
        spdlog::set_level(spdlog::level::debug);
        LOG_DEBUG("DEBUG 级别应该显示");
        LOG_INFO("INFO 级别应该显示");
        LOG_TRACE("TRACE 级别不应该显示");
        
        std::cout << "\n--- 设置为 WARN 级别 ---" << std::endl;
        spdlog::set_level(spdlog::level::warn);
        LOG_DEBUG("DEBUG 级别不应该显示");
        LOG_INFO("INFO 级别不应该显示");
        LOG_WARN("WARN 级别应该显示");
        LOG_ERROR("ERROR 级别应该显示");
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "日志初始化失败: " << ex.what() << std::endl;
    }
}

void test_format_patterns() {
    std::cout << "\n========== 测试 5: 格式化模式 ==========" << std::endl;
    
    try {
        auto logger = spdlog::stdout_color_mt("format_test");
        spdlog::set_default_logger(logger);
        
        // 模式 1: 完整格式
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] [%s:%#] %v");
        LOG_INFO("完整格式: 时间+线程+级别+文件+行号");
        
        // 模式 2: 简洁格式
        logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
        LOG_INFO("简洁格式: 时间+级别");
        
        // 模式 3: 自定义格式
        logger->set_pattern("%Y-%m-%d %v");
        LOG_INFO("自定义格式: 日期+消息");
        
        // 测试各种数据类型格式化
        logger->set_pattern("[%l] %v");
        LOG_INFO("整数: {}, 十六进制: {:#x}", 255, 255);
        LOG_INFO("浮点数: {:.3f}, 科学计数法: {:.2e}", 3.14159, 12345.6789);
        LOG_INFO("字符串: {}, 字符填充: {:*^10}", "test", "hi");
        LOG_INFO("布尔值: {}, {}", true, false);
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "日志初始化失败: " << ex.what() << std::endl;
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  spdlog 裁剪测试 Demo" << std::endl;
    std::cout << "  版本: 1.17.0 (裁剪版)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 确保日志目录存在
    system("mkdir -p logs");
    
    // 运行测试
    test_sync_console();
    test_sync_file();
    test_async_multi_sink();
    test_level_filter();
    test_format_patterns();
    
    // 关闭 spdlog
    spdlog::shutdown();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  所有测试完成!" << std::endl;
    std::cout << "  日志文件保存在 logs/ 目录" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}