#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace common::process {
enum class ConsoleMode { Inherit, NewConsole };
enum class WindowMode { Default, Normal, Minimized, Maximized, Hidden };
enum class TerminalMode { None, NewTerminal };
struct ProcessOptions {
    std::filesystem::path executable;
    std::vector<std::string> args;
    std::filesystem::path working_dir;
    ConsoleMode console_mode{ConsoleMode::Inherit};
    WindowMode window_mode{WindowMode::Default};
    TerminalMode terminal_mode{TerminalMode::None};
    std::string terminal_title;
};

// Linux backend: std::error_code, posix_spawnp, signals and a reaping thread.
// Lifecycle methods are caller-serialized. Wait/signal/query may run concurrently.
// AsyncWait runs on the reaper (or inline if already finished); it has no Asio executor.
// Callbacks must not throw. Reset/destruction kills and reaps a running child.
class Process {
public:
    using ExitHandler = std::function<void(const std::error_code&, int)>;
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    bool Start(const ProcessOptions& options);
    bool Start(const ProcessOptions& options, std::error_code& ec) noexcept;
    bool IsOpen() const noexcept;
    bool IsRunning();
    bool IsRunning(std::error_code& ec) noexcept;
    std::uint64_t Pid() const noexcept;
    int ExitCode() const noexcept;
    void AsyncWait(ExitHandler handler);
    bool RequestExit(std::error_code& ec) noexcept;
    bool Terminate(std::error_code& ec) noexcept;
    int Wait();
    int Wait(std::error_code& ec) noexcept;
    void Reset() noexcept;
    const std::string& LastError() const noexcept;
private:
    class ProcessImpl;
    std::unique_ptr<ProcessImpl> impl_;
};
} // namespace common::process
