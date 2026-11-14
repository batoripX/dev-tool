module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module dev.command;

import std;

export namespace dev::command {

enum class ErrorKind {
    InvalidDirectory,
    EmptyCommand,
    WatchFailure,
    ProcessSpawnFailure
};

struct Error {
    ErrorKind kind{};
    std::uint32_t win32_code{0};
    std::string message{};
    std::string hint{};
};

struct WatchConfig {
    std::filesystem::path watch_dir{"."};
    std::string command_line{};
    std::chrono::milliseconds debounce{120};
    bool run_immediately{true};
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] auto format_win32_error(std::uint32_t code) -> std::string {
    LPSTR buffer = nullptr;
    auto&& flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    auto&& len = ::FormatMessageA(flags, nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (len == 0 || buffer == nullptr) {
        return std::format("Win32 Error 0x{:08X}", code);
    }
    auto msg = std::string(buffer, len);
    ::LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

[[nodiscard]] auto is_ignored_artifact(std::wstring_view rel_path) noexcept -> bool {
    return rel_path.ends_with(L".pcm") || rel_path.ends_with(L".obj") ||
           rel_path.ends_with(L".exe") || rel_path.ends_with(L".tmp") ||
           rel_path.find(L".git") != std::wstring_view::npos ||
           rel_path.find(L"build\\") != std::wstring_view::npos;
}

[[nodiscard]] auto execute_shell_command(std::string_view cmd, const std::filesystem::path& work_dir) -> Result<std::uint32_t> {
    auto full_cmd = std::format("pwsh.exe -NoProfile -Command \"{}\"", cmd);
    auto mutable_cmd = std::vector<char>(full_cmd.begin(), full_cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    auto work_dir_str = work_dir.string();
    auto&& ok = ::CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        work_dir_str.c_str(),
        &si,
        &pi
    );

    if (!ok) {
        auto&& err = ::GetLastError();
        return std::unexpected(Error{
            .kind = ErrorKind::ProcessSpawnFailure,
            .win32_code = err,
            .message = std::format("CreateProcessA failed for command '{}': {}", cmd, format_win32_error(err)),
            .hint = "Ensure pwsh.exe (PowerShell 7) is on your system PATH."
        });
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    return static_cast<std::uint32_t>(exit_code);
}

[[nodiscard]] auto watch_and_run(const WatchConfig& cfg) -> Result<void> {
    if (cfg.command_line.empty()) {
        return std::unexpected(Error{
            .kind = ErrorKind::EmptyCommand,
            .message = "No command provided to watch runner.",
            .hint = "Pass --cmd \"clang++ ...\" to execute on file save."
        });
    }

    std::error_code ec{};
    if (!std::filesystem::exists(cfg.watch_dir, ec) || !std::filesystem::is_directory(cfg.watch_dir, ec)) {
        return std::unexpected(Error{
            .kind = ErrorKind::InvalidDirectory,
            .win32_code = ERROR_PATH_NOT_FOUND,
            .message = std::format("Watch directory does not exist: '{}'", cfg.watch_dir.string()),
            .hint = "Check the target directory path."
        });
    }

    auto canonical_dir = std::filesystem::weakly_canonical(cfg.watch_dir, ec);
    auto wide_dir = canonical_dir.wstring();

    auto&& dir_handle = ::CreateFileW(
        wide_dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (dir_handle == INVALID_HANDLE_VALUE) {
        auto&& err = ::GetLastError();
        return std::unexpected(Error{
            .kind = ErrorKind::WatchFailure,
            .win32_code = err,
            .message = std::format("CreateFileW failed on watch directory: {}", format_win32_error(err)),
            .hint = "Verify read permissions on the watched directory."
        });
    }

    if (cfg.run_immediately) {
        std::println("[watch] Initial run -> {}", cfg.command_line);
        if (auto res = execute_shell_command(cfg.command_line, canonical_dir); !res) {
            ::CloseHandle(dir_handle);
            return std::unexpected(res.error());
        } else {
            std::println("[watch] Exit code: {}", *res);
        }
    }

    std::println("[watch] Watching '{}' (Ctrl+C to stop)...", canonical_dir.string());

    alignas(DWORD) std::array<std::byte, 64 * 1024> notify_buf{};
    DWORD bytes_returned = 0;
    auto last_trigger = std::chrono::steady_clock::now() - cfg.debounce;

    while (true) {
        auto&& ok = ::ReadDirectoryChangesW(
            dir_handle,
            notify_buf.data(),
            static_cast<DWORD>(notify_buf.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION,
            &bytes_returned,
            nullptr,
            nullptr
        );

        if (!ok) {
            auto&& err = ::GetLastError();
            ::CloseHandle(dir_handle);
            return std::unexpected(Error{
                .kind = ErrorKind::WatchFailure,
                .win32_code = err,
                .message = std::format("ReadDirectoryChangesW failed: {}", format_win32_error(err)),
                .hint = "The watched directory may have been deleted or unmounted."
            });
        }

        bool relevant_change = false;
        std::string changed_file{};
        auto* offset = notify_buf.data();

        while (true) {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(offset);
            auto filename_len = info->FileNameLength / sizeof(WCHAR);
            auto rel_wstr = std::wstring_view(info->FileName, filename_len);

            if (!is_ignored_artifact(rel_wstr)) {
                relevant_change = true;
                changed_file = std::filesystem::path(rel_wstr).string();
            }

            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }

        auto now = std::chrono::steady_clock::now();
        if (relevant_change && (now - last_trigger) >= cfg.debounce) {
            last_trigger = now;
            std::println("\n[watch] Change detected: {} -> Running build...", changed_file);
            auto run_res = execute_shell_command(cfg.command_line, canonical_dir);
            if (!run_res) {
                ::CloseHandle(dir_handle);
                return std::unexpected(run_res.error());
            }
            std::println("[watch] Completed with exit code: {}", *run_res);
        }
    }
}

} // namespace dev::command
