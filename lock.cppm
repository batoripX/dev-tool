module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <restartmanager.h>

export module dev.lock;

import std;

export namespace dev::lock {

enum class ErrorKind {
    InvalidTarget,
    Win32ApiFailure,
    AccessDenied,
    NotFound
};

struct Error {
    ErrorKind kind{};
    std::uint32_t win32_code{0};
    std::string message{};
    std::string hint{};
};

struct LockHolder {
    std::uint32_t pid{0};
    std::string process_name{};
    std::string full_path{};
    std::string detail{};
};

template <typename T>
using Result = std::expected<T, Error>;
[[nodiscard]] auto format_win32_error(std::uint32_t code) -> std::string {
    if (code == 0) {
        return "Success";
    }
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

[[nodiscard]] auto tcp_state_name(DWORD state) noexcept -> std::string_view {
    switch (state) {
        case MIB_TCP_STATE_CLOSED:     return "CLOSED";
        case MIB_TCP_STATE_LISTEN:     return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT:   return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD:   return "SYN_RCVD";
        case MIB_TCP_STATE_ESTAB:      return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1:  return "FIN_WAIT1";
        case MIB_TCP_STATE_FIN_WAIT2:  return "FIN_WAIT2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING:    return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK:   return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT:  return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
        default:                       return "UNKNOWN";
    }
}

[[nodiscard]] auto query_process_info(std::uint32_t pid) -> std::pair<std::string, std::string> {
    if (pid == 0) return {"System Idle Process", "System"};
    if (pid == 4) return {"System", "ntoskrnl.exe"};

    auto&& handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return {"<protected/elevated>", "<access denied - run as admin>"};
    }

    char path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    auto&& ok = ::QueryFullProcessImageNameA(handle, 0, path, &size);
    ::CloseHandle(handle);

    if (!ok) {
        return {"<unknown>", "<unknown>"};
    }
    auto full = std::string(path, size);
    auto name = std::filesystem::path(full).filename().string();
    return {std::move(name), std::move(full)};
}
[[nodiscard]] auto find_port_holders(std::uint16_t port) -> Result<std::vector<LockHolder>> {
    auto holders = std::vector<LockHolder>{};

    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        DWORD size = 0;
        ::GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
        if (size == 0) continue;

        auto buf = std::vector<std::byte>(size);
        auto&& res = ::GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
        if (res != NO_ERROR) {
            return std::unexpected(Error{
                .kind = ErrorKind::Win32ApiFailure,
                .win32_code = res,
                .message = std::format("GetExtendedTcpTable (family={}) failed: {}", family, format_win32_error(res)),
                .hint = "Ensure Windows IP Helper service (iphlpsvc) is active."
            });
        }

        if (family == AF_INET) {
            auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto&& row = table->table[i];
                auto&& local_port = ::ntohs(static_cast<u_short>(row.dwLocalPort));
                if (local_port == port) {
                    auto [name, full] = query_process_info(row.dwOwningPid);
                    holders.push_back(LockHolder{row.dwOwningPid, std::move(name), std::move(full),
                        std::format("TCPv4 :{} [{}]", local_port, tcp_state_name(row.dwState))});
                }
            }
        } else {
            auto* table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto&& row = table->table[i];
                auto&& local_port = ::ntohs(static_cast<u_short>(row.dwLocalPort));
                if (local_port == port) {
                    auto [name, full] = query_process_info(row.dwOwningPid);
                    holders.push_back(LockHolder{row.dwOwningPid, std::move(name), std::move(full),
                        std::format("TCPv6 :{} [{}]", local_port, tcp_state_name(row.dwState))});
                }
            }
        }
    }

    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        DWORD size = 0;
        ::GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
        if (size == 0) continue;

        auto buf = std::vector<std::byte>(size);
        if (::GetExtendedUdpTable(buf.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) continue;

        if (family == AF_INET) {
            auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto&& row = table->table[i];
                auto&& local_port = ::ntohs(static_cast<u_short>(row.dwLocalPort));
                if (local_port == port) {
                    auto [name, full] = query_process_info(row.dwOwningPid);
                    holders.push_back(LockHolder{row.dwOwningPid, std::move(name), std::move(full),
                        std::format("UDPv4 :{}", local_port)});
                }
            }
        } else {
            auto* table = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto&& row = table->table[i];
                auto&& local_port = ::ntohs(static_cast<u_short>(row.dwLocalPort));
                if (local_port == port) {
                    auto [name, full] = query_process_info(row.dwOwningPid);
                    holders.push_back(LockHolder{row.dwOwningPid, std::move(name), std::move(full),
                        std::format("UDPv6 :{}", local_port)});
                }
            }
        }
    }

    return holders;
}
[[nodiscard]] auto find_file_holders(const std::filesystem::path& target) -> Result<std::vector<LockHolder>> {
    std::error_code ec{};
    if (!std::filesystem::exists(target, ec)) {
        return std::unexpected(Error{ErrorKind::NotFound, ERROR_FILE_NOT_FOUND,
            std::format("Target file does not exist: '{}'", target.string()),
            "Check the file path or verify it wasn't already removed."});
    }
    if (std::filesystem::is_directory(target, ec)) {
        return std::unexpected(Error{ErrorKind::InvalidTarget, ERROR_DIRECTORY,
            std::format("Target '{}' is a directory, not a file.", target.string()),
            "Pass a specific locked binary or artifact inside that folder (e.g. 'build/app.exe')."});
    }

    auto canonical = std::filesystem::weakly_canonical(target, ec);
    auto wide_path = canonical.wstring();
    LPCWSTR resources[] = { wide_path.c_str() };

    DWORD session = 0;
    WCHAR session_key[CCH_RM_SESSION_KEY + 1]{};
    if (auto&& res = ::RmStartSession(&session, 0, session_key); res != ERROR_SUCCESS) {
        return std::unexpected(Error{ErrorKind::Win32ApiFailure, res,
            std::format("RmStartSession failed: {}", format_win32_error(res)),
            "Restart Manager could not allocate a session handle."});
    }

    if (auto&& res = ::RmRegisterResources(session, 1, resources, 0, nullptr, 0, nullptr); res != ERROR_SUCCESS) {
        ::RmEndSession(session);
        return std::unexpected(Error{ErrorKind::Win32ApiFailure, res,
            std::format("RmRegisterResources failed: {}", format_win32_error(res)),
            "Ensure the file path is accessible."});
    }

    UINT needed = 0;
    UINT count = 0;
    DWORD reboot_reasons = RmRebootReasonNone;
    auto holders = std::vector<LockHolder>{};

    if (::RmGetList(session, &needed, &count, nullptr, &reboot_reasons) == ERROR_MORE_DATA && needed > 0) {
        auto proc_info = std::vector<RM_PROCESS_INFO>(needed);
        count = needed;
        if (::RmGetList(session, &needed, &count, proc_info.data(), &reboot_reasons) == ERROR_SUCCESS) {
            for (UINT i = 0; i < count; ++i) {
                auto&& pid = proc_info[i].Process.dwProcessId;
                auto [name, full] = query_process_info(pid);
                holders.push_back(LockHolder{pid, std::move(name), std::move(full),
                    std::format("File lock on '{}'", canonical.filename().string())});
            }
        }
    }

    ::RmEndSession(session);
    return holders;
}

[[nodiscard]] auto kill_pid(std::uint32_t pid) -> Result<void> {
    if (pid <= 4) {
        return std::unexpected(Error{ErrorKind::AccessDenied, ERROR_ACCESS_DENIED,
            std::format("Refusing to terminate critical kernel/system PID {}", pid),
            "Only user-space developer processes can be sniped."});
    }
    auto&& handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (handle == nullptr) {
        auto&& err = ::GetLastError();
        return std::unexpected(Error{ErrorKind::AccessDenied, err,
            std::format("OpenProcess(PROCESS_TERMINATE) failed for PID {}: {}", pid, format_win32_error(err)),
            "Re-run PowerShell 7 as Administrator if the target process is elevated."});
    }
    auto&& ok = ::TerminateProcess(handle, 1);
    auto&& err = ::GetLastError();
    ::CloseHandle(handle);
    if (!ok) {
        return std::unexpected(Error{ErrorKind::Win32ApiFailure, err,
            std::format("TerminateProcess failed for PID {}: {}", pid, format_win32_error(err)),
            "Target process may be protected by PPL/Antimalware."});
    }
    return {};
}

[[nodiscard]] auto inspect_or_snipe(std::string_view target, bool kill) -> Result<std::vector<LockHolder>> {
    if (target.empty()) {
        return std::unexpected(Error{ErrorKind::InvalidTarget, 0,
            "Empty target passed to lock command.",
            "Specify ':3000' for a port or 'build/app.exe' for a locked file."});
    }

    auto holders_res = Result<std::vector<LockHolder>>{};
    if (target.starts_with(':')) {
        auto port_sv = target.substr(1);
        std::uint16_t port = 0;
        auto [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), port);
        if (ec != std::errc{} || ptr != port_sv.data() + port_sv.size() || port == 0) {
            return std::unexpected(Error{ErrorKind::InvalidTarget, 0,
                std::format("Invalid port number '{}'", target),
                "Port must be between 1 and 65535 (e.g. ':8080')."});
        }
        holders_res = find_port_holders(port);
    } else {
        holders_res = find_file_holders(std::filesystem::path(target));
    }

    if (!holders_res) {
        return std::unexpected(holders_res.error());
    }

    auto holders = std::move(*holders_res);
    if (kill && !holders.empty()) {
        auto seen_pids = std::unordered_set<std::uint32_t>{};
        for (auto&& h : holders) {
            if (seen_pids.insert(h.pid).second) {
                if (auto kill_res = kill_pid(h.pid); !kill_res) {
                    return std::unexpected(kill_res.error());
                }
            }
        }
    }
    return holders;
}

} // namespace dev::lock
