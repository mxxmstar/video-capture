#include "common/process/process.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
using namespace common::process;
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main() {
    try {
        Process p;
        ProcessOptions opt;
        std::error_code ec;
        opt.executable = "/definitely/missing/executable";
        Require(!p.Start(opt, ec) && bool(ec), "missing executable accepted");
        opt.executable = "/bin/sh";
        opt.working_dir = "/tmp";
        opt.args = {"-c", "test \"$PWD\" = /tmp && exit 7"};
        Require(p.Start(opt, ec), "start with working directory failed");
        Require(p.Wait(ec) == 7 && !ec, "exit status or working directory lost");
        Require(p.Wait(ec) == 7 && !p.IsRunning(), "cached wait failed");
        opt.working_dir = "/definitely/missing/directory";
        Require(!p.Start(opt, ec) && bool(ec), "missing working directory accepted");
        opt.working_dir.clear();
        opt.args = {"-c", "exec sleep 10"};
        Require(p.Start(opt, ec), "sleep start failed");
        Require(!p.Start(opt, ec) && bool(ec), "double start accepted");
        std::promise<int> completion;
        auto future = completion.get_future();
        p.AsyncWait([&](const std::error_code& error, int code) {
            completion.set_value(error ? -1 : code);
        });
        Require(p.RequestExit(ec), "SIGTERM failed");
        Require(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "async completion timed out");
        Require(future.get() == 143 && p.Wait(ec) == 143, "signal status lost");
        p.Reset();
        Require(!p.IsOpen(), "reset did not close process");
        int child = 0;
        { Process owned; Require(owned.Start(opt, ec), "owned start failed"); child = owned.Pid(); }
        Require(waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD, "destructor left child unreaped");
        std::cout << "POSIX process lifecycle, error handling, async wait and cleanup passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
