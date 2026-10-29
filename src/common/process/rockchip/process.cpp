#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common/process/process.h"
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
extern char** environ;

namespace common::process {
class Process::ProcessImpl {
public:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        pid_t pid{0};
        bool done{false};
        int code{-1};
        std::error_code error;
        std::vector<ExitHandler> handlers;
    };
    std::shared_ptr<State> state;
    std::thread reaper;
    std::string last_error;
    ~ProcessImpl() { Reset(); }

    bool Fail(int code, std::error_code& ec) {
        ec = std::error_code(code, std::generic_category());
        last_error = ec.message();
        return false;
    }
    bool Start(const ProcessOptions& options, std::error_code& ec) {
        if (options.executable.empty()) return Fail(EINVAL, ec);
        if (state) {
            std::lock_guard lock(state->mutex);
            if (!state->done) return Fail(EBUSY, ec);
        }
        if (options.terminal_mode != TerminalMode::None ||
            options.console_mode != ConsoleMode::Inherit ||
            options.window_mode != WindowMode::Default) return Fail(ENOTSUP, ec);
        Reset();
        auto next = std::make_shared<State>();
        std::vector<std::string> args{options.executable.string()};
        args.insert(args.end(), options.args.begin(), options.args.end());
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        int ret = posix_spawn_file_actions_init(&actions);
        if (ret) return Fail(ret, ec);
        if (!options.working_dir.empty())
            ret = posix_spawn_file_actions_addchdir_np(&actions, options.working_dir.c_str());
        if (!ret) ret = posix_spawnp(&next->pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (ret) return Fail(ret, ec);
        try {
            reaper = std::thread([next] {
                // WNOWAIT leaves the child unreaped until we hold the signal mutex,
                // preventing a concurrent kill from acting on a recycled PID.
                siginfo_t info{};
                int result;
                do { result = waitid(P_PID, next->pid, &info, WEXITED | WNOWAIT); }
                while (result < 0 && errno == EINTR);
                const int wait_error = result < 0 ? errno : 0;
                std::vector<ExitHandler> handlers;
                {
                    std::lock_guard lock(next->mutex);
                    int status = 0;
                    pid_t waited = -1;
                    if (!wait_error) {
                        do { waited = waitpid(next->pid, &status, 0); }
                        while (waited < 0 && errno == EINTR);
                    }
                    if (wait_error || waited < 0)
                        next->error = std::error_code(wait_error ? wait_error : errno, std::generic_category());
                    else if (WIFEXITED(status)) next->code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) next->code = 128 + WTERMSIG(status);
                    next->pid = 0;
                    next->done = true;
                    handlers.swap(next->handlers);
                }
                next->cv.notify_all();
                for (auto& handler : handlers) handler(next->error, next->code);
            });
        } catch (...) {
            kill(next->pid, SIGKILL);
            while (waitpid(next->pid, nullptr, 0) < 0 && errno == EINTR) {}
            throw;
        }
        state = std::move(next);
        ec.clear();
        last_error.clear();
        return true;
    }
    bool Signal(int signal, std::error_code& ec) {
        if (!state) return Fail(ESRCH, ec);
        std::lock_guard lock(state->mutex);
        if (state->done || state->pid <= 0) return Fail(ESRCH, ec);
        if (kill(state->pid, signal) < 0) return Fail(errno, ec);
        ec.clear(); return true;
    }
    int Wait(std::error_code& ec) {
        if (!state) { Fail(ECHILD, ec); return -1; }
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->done; });
        ec = state->error;
        return state->code;
    }
    void Reset() noexcept {
        if (state) {
            std::error_code ignored;
            Signal(SIGKILL, ignored);
        }
        if (reaper.joinable()) {
            // A completion callback may destroy its Process after the child is reaped.
            if (reaper.get_id() == std::this_thread::get_id()) reaper.detach();
            else reaper.join();
        }
        state.reset();
    }
};
Process::Process() : impl_(std::make_unique<ProcessImpl>()) {}
Process::~Process() = default;
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;
bool Process::Start(const ProcessOptions& options) {
    std::error_code ec; const bool ok = Start(options, ec);
    if (ec) throw std::system_error(ec, "process start");
    return ok;
}
bool Process::Start(const ProcessOptions& options, std::error_code& ec) noexcept {
    try { return impl_->Start(options, ec); }
    catch (const std::system_error& e) { ec = e.code(); }
    catch (...) { ec = std::make_error_code(std::errc::not_enough_memory); }
    return false;
}
bool Process::IsOpen() const noexcept { return impl_ && bool(impl_->state); }
bool Process::IsRunning() { std::error_code ec; return IsRunning(ec); }
bool Process::IsRunning(std::error_code& ec) noexcept {
    ec.clear();
    if (!IsOpen()) return false;
    std::lock_guard lock(impl_->state->mutex); return !impl_->state->done;
}
std::uint64_t Process::Pid() const noexcept {
    if (!IsOpen()) return 0;
    std::lock_guard lock(impl_->state->mutex); return impl_->state->pid;
}
int Process::ExitCode() const noexcept {
    if (!IsOpen()) return -1;
    std::lock_guard lock(impl_->state->mutex); return impl_->state->code;
}
void Process::AsyncWait(ExitHandler handler) {
    if (!handler) return;
    if (!IsOpen()) { handler(std::make_error_code(std::errc::no_child_process), -1); return; }
    auto state = impl_->state;
    std::unique_lock lock(state->mutex);
    if (!state->done) { state->handlers.push_back(std::move(handler)); return; }
    auto ec = state->error; int code = state->code;
    lock.unlock(); handler(ec, code);
}
bool Process::RequestExit(std::error_code& ec) noexcept { return impl_->Signal(SIGTERM, ec); }
bool Process::Terminate(std::error_code& ec) noexcept { return impl_->Signal(SIGKILL, ec); }
int Process::Wait() {
    std::error_code ec; int code = Wait(ec);
    if (ec) throw std::system_error(ec, "process wait");
    return code;
}
int Process::Wait(std::error_code& ec) noexcept { return impl_->Wait(ec); }
void Process::Reset() noexcept { if (impl_) impl_->Reset(); }
const std::string& Process::LastError() const noexcept { return impl_->last_error; }
} // namespace common::process
