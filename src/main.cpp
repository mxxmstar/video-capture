#include <iostream>

#include "media/stream/stream_session.h"
#include "media/stream/stream_source.h"
#include "media/puller/ffmpeg_puller.h"
#include <asio.hpp>

struct IOTestContext {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread thread;

    IOTestContext()
        : work(asio::make_work_guard(io))
        , thread([this]() { io.run(); }) {}

    ~IOTestContext() {
        work.reset();
        if (thread.joinable())
            thread.join();
    }
};

int main()
{
    std::cout << "Hello World!\n";
    std::string url = "rtsp://192.168.66.219/live/mainstream";

    auto puller = std::make_unique<FFmpegPuller>();
    puller->SetConnectTimeoutMs(1000);
    puller->SetReadTimeoutMs(1000);
    puller->SetLowLatency(true);

    IOTestContext io_ctx;
    auto session = std::make_shared<MediaStreamSession>(io_ctx.io);
    session->SetPuller(std::move(puller));
    session->SetUrl(url);

    auto source = std::make_shared<MediaStreamSource>("test_stream");
    source->SetSession(session);

    try {
        bool ret = source->Start();
        std::cout << "Stream started: " << (ret ? "true" : "false") << std::endl;

        if (ret) {
            // 启动定时打印，每5秒打印一次音视频流统计
            source->StartStatsPrint(5);

            // 阻塞主线程，等待用户输入退出
            std::cout << "Press Enter to stop..." << std::endl;
            std::cin.get();

            source->StopStatsPrint();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}