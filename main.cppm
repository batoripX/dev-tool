module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module dev.main;

import std;
import dev.lock;
import dev.build;
import dev.command;

namespace {

auto enable_vt_console() noexcept -> void {
    auto&& hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD mode = 0;
        if (::GetConsoleMode(hOut, &mode)) {
            ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    auto&& hErr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE && hErr != nullptr) {
        DWORD mode = 0;
        if (::GetConsoleMode(hErr, &mode)) {
            ::SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

auto print_banner() -> void {
    std::println("dev - Native Win32 C++23/26 Developer Swiss-Army CLI");
    std::println("Usage:");
    std::println("  dev lock :<port> [--kill]               Inspect or kill process holding a TCP/UDP port");
    std::println("  dev lock <file-path> [--kill]           Inspect or kill process locking a file");
    std::println("  dev sweep [dir] [--dry-run] [--exe]     Purge .pcm, .obj, build, target, node_modules");
    std::println("  dev watch [dir] --cmd \"<command>\"       Live-run command when files change");
}

template <typename Err>
auto report_error(std::string_view subsystem, const Err& err) -> int {
    std::println(std::cerr, "\x1b[31m[error:{}]\x1b[0m {}", subsystem, err.message);
    if (err.win32_code != 0) {
        std::println(std::cerr, "  \x1b[90mwin32_code:\x1b[0m 0x{:08X} ({})", err.win32_code, err.win32_code);
    }
    if (!err.hint.empty()) {
        std::println(std::cerr, "  \x1b[33mhint:\x1b[0m {}", err.hint);
    }
    return 1;
}

auto handle_lock(std::span<const std::string_view> args) -> int {
    if (args.empty()) {
        std::println(std::cerr, "\x1b[31m[error:lock]\x1b[0m Missing target port or file path.");
        std::println(std::cerr, "  \x1b[33mhint:\x1b[0m Try 'dev lock :3000' or 'dev lock app.exe --kill'");
        return 1;
    }

    auto target = args[0];
    bool kill = false;
    for (auto&& arg : args.subspan(1)) {
        if (arg == "--kill" || arg == "-k") {
            kill = true;
        }
    }

    auto res = dev::lock::inspect_or_snipe(target, kill);
    if (!res) {
        return report_error("lock", res.error());
    }

    if (res->empty()) {
        std::println("[lock] Clean: No processes holding '{}'.", target);
        return 0;
    }

    for (auto&& h : *res) {
        std::println("[lock] PID {:<6} | {:<24} | {} {}",
                     h.pid, h.process_name, h.detail, kill ? "[TERMINATED]" : "");
    }
    return 0;
}
auto handle_sweep(std::span<const std::string_view> args) -> int {
    auto opts = dev::build::SweepOptions{};
    for (auto&& arg : args) {
        if (arg == "--dry-run" || arg == "-n") {
            opts.dry_run = true;
        } else if (arg == "--exe" || arg == "--include-exe") {
            opts.include_exe = true;
        } else if (!arg.starts_with('-')) {
            opts.root = std::filesystem::path(arg);
        }
    }

    auto res = dev::build::run_sweep(opts);
    if (!res) {
        return report_error("sweep", res.error());
    }

    auto mb = static_cast<double>(res->bytes_reclaimed) / (1024.0 * 1024.0);
    std::println("[sweep{}] Removed {} dirs, {} files ({:.2f} MB reclaimed).",
                 opts.dry_run ? ":dry-run" : "",
                 res->dirs_removed, res->files_removed, mb);

    if (!res->skipped_locked.empty()) {
        std::println("\x1b[33m[sweep:warning]\x1b[0m {} locked items skipped (use 'dev lock <path> --kill'):",
                     res->skipped_locked.size());
        for (auto&& item : res->skipped_locked) {
            std::println("  - {}", item);
        }
    }
    return 0;
}

auto handle_watch(std::span<const std::string_view> args) -> int {
    auto cfg = dev::command::WatchConfig{};
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--cmd" || args[i] == "-c") && (i + 1) < args.size()) {
            cfg.command_line = std::string(args[++i]);
        } else if (args[i] == "--no-initial") {
            cfg.run_immediately = false;
        } else if (!args[i].starts_with('-')) {
            cfg.watch_dir = std::filesystem::path(args[i]);
        }
    }

    auto res = dev::command::watch_and_run(cfg);
    if (!res) {
        return report_error("watch", res.error());
    }
    return 0;
}

} // namespace

export extern "C++" int main(int argc, char* argv[]) {
    enable_vt_console();

    auto args = std::vector<std::string_view>{};
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
        print_banner();
        return 0;
    }

    auto subcommand = args[1];
    auto tail = std::span<const std::string_view>(args).subspan(2);

    if (subcommand == "lock") {
        return handle_lock(tail);
    }
    if (subcommand == "sweep") {
        return handle_sweep(tail);
    }
    if (subcommand == "watch") {
        return handle_watch(tail);
    }

    std::println(std::cerr, "\x1b[31m[error:cli]\x1b[0m Unknown subcommand '{}'", subcommand);
    print_banner();
    return 1;
}
