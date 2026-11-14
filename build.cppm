module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module dev.build;

import std;

export namespace dev::build {

enum class ErrorKind {
    InvalidRoot,
    Win32ApiFailure,
    PartialCleanup
};

struct Error {
    ErrorKind kind{};
    std::uint32_t win32_code{0};
    std::string message{};
    std::string hint{};
};

struct SweepOptions {
    std::filesystem::path root{"."};
    bool dry_run{false};
    bool include_exe{false};
};

struct SweepReport {
    std::uint64_t dirs_removed{0};
    std::uint64_t files_removed{0};
    std::uint64_t bytes_reclaimed{0};
    std::vector<std::string> skipped_locked{};
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

[[nodiscard]] auto is_target_artifact_dir(std::wstring_view name) noexcept -> bool {
    return name == L"build" || name == L"node_modules" || name == L"target" ||
           name == L"CMakeFiles" || name == L".cache";
}

[[nodiscard]] auto is_target_artifact_file(std::wstring_view name, bool include_exe) noexcept -> bool {
    auto dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos) {
        return false;
    }
    auto ext = name.substr(dot);
    if (ext == L".pcm" || ext == L".obj" || ext == L".o" || ext == L".ilk" || ext == L".pdb") {
        return true;
    }
    if (include_exe && ext == L".exe") {
        return true;
    }
    return false;
}

auto purge_tree_win32(const std::filesystem::path& dir, bool dry_run, SweepReport& report) -> void {
    auto pattern = (dir / L"*").wstring();
    WIN32_FIND_DATAW ffd{};
    auto&& hFind = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &ffd,
                                      FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        auto name = std::wstring_view(ffd.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        auto full_path = dir / name;
        auto full_wstr = full_path.wstring();

        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (!dry_run) {
                ::RemoveDirectoryW(full_wstr.c_str());
            }
            ++report.dirs_removed;
            continue;
        }

        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            purge_tree_win32(full_path, dry_run, report);
            if (!dry_run) {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    ::SetFileAttributesW(full_wstr.c_str(), ffd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                if (!::RemoveDirectoryW(full_wstr.c_str())) {
                    report.skipped_locked.push_back(full_path.string());
                    continue;
                }
            }
            ++report.dirs_removed;
        } else {
            auto size = (static_cast<std::uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
            if (!dry_run) {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    ::SetFileAttributesW(full_wstr.c_str(), ffd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                if (!::DeleteFileW(full_wstr.c_str())) {
                    report.skipped_locked.push_back(full_path.string());
                    continue;
                }
            }
            ++report.files_removed;
            report.bytes_reclaimed += size;
        }
    } while (::FindNextFileW(hFind, &ffd) != 0);

    ::FindClose(hFind);
}

auto scan_and_sweep(const std::filesystem::path& dir, const SweepOptions& opts, SweepReport& report) -> void {
    auto pattern = (dir / L"*").wstring();
    WIN32_FIND_DATAW ffd{};
    auto&& hFind = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &ffd,
                                      FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        auto name = std::wstring_view(ffd.cFileName);
        if (name == L"." || name == L".." || name == L".git") {
            continue;
        }
        auto full_path = dir / name;
        auto full_wstr = full_path.wstring();

        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (is_target_artifact_dir(name)) {
                purge_tree_win32(full_path, opts.dry_run, report);
                if (!opts.dry_run) {
                    if (::RemoveDirectoryW(full_wstr.c_str())) {
                        ++report.dirs_removed;
                    } else {
                        report.skipped_locked.push_back(full_path.string());
                    }
                } else {
                    ++report.dirs_removed;
                }
            } else if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                scan_and_sweep(full_path, opts, report);
            }
        } else if (is_target_artifact_file(name, opts.include_exe)) {
            auto size = (static_cast<std::uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
            if (!opts.dry_run) {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    ::SetFileAttributesW(full_wstr.c_str(), ffd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                if (!::DeleteFileW(full_wstr.c_str())) {
                    report.skipped_locked.push_back(full_path.string());
                    continue;
                }
            }
            ++report.files_removed;
            report.bytes_reclaimed += size;
        }
    } while (::FindNextFileW(hFind, &ffd) != 0);

    ::FindClose(hFind);
}

[[nodiscard]] auto run_sweep(const SweepOptions& opts) -> Result<SweepReport> {
    std::error_code ec{};
    if (!std::filesystem::exists(opts.root, ec) || !std::filesystem::is_directory(opts.root, ec)) {
        return std::unexpected(Error{
            .kind = ErrorKind::InvalidRoot,
            .win32_code = ERROR_PATH_NOT_FOUND,
            .message = std::format("Sweep root is not a valid directory: '{}'", opts.root.string()),
            .hint = "Pass a valid project directory or omit the path to sweep the current directory."
        });
    }

    auto canonical_root = std::filesystem::weakly_canonical(opts.root, ec);
    auto report = SweepReport{};
    scan_and_sweep(canonical_root, opts, report);
    return report;
}

} // namespace dev::build
