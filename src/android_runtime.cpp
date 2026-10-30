#include "sifa/android_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace sifa::android {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Environment = std::map<std::string, std::string>;
constexpr std::size_t maximum_ini_size = 256 * 1024;
constexpr std::size_t maximum_output = 64 * 1024;
constexpr std::size_t maximum_log = 4 * 1024 * 1024;

std::string path_text(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path utf8_path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (!needed || needed > 32768) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), needed);
    if (!written || written >= needed) return {};
    value.resize(written);
    return path_text(fs::path(value));
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

fs::path user_home() {
#ifdef _WIN32
    return utf8_path(environment("USERPROFILE"));
#else
    return utf8_path(environment("HOME"));
#endif
}

fs::path avd_home() {
    for (const char* key : {"ANDROID_AVD_HOME", "ANDROID_USER_HOME", "ANDROID_EMULATOR_HOME"}) {
        const auto value = environment(key);
        if (!value.empty()) return key == std::string("ANDROID_AVD_HOME")
            ? utf8_path(value) : utf8_path(value) / "avd";
    }
    const auto legacy_home = environment("ANDROID_SDK_HOME");
    if (!legacy_home.empty()) return utf8_path(legacy_home) / ".android" / "avd";
    const auto home = user_home();
    return home.empty() ? fs::path{} : home / ".android" / "avd";
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool valid_avd_name(const std::string& name) {
    if (name.empty() || name.size() > 80 || name == "." || name == ".." || name[0] == '-') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

bool read_ini(const fs::path& path, std::map<std::string, std::string>& values) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || fs::file_size(path, ec) > maximum_ini_size || ec) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    // Read at most the bound even if the file grows after file_size().
    std::string text(maximum_ini_size + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.size() > maximum_ini_size || text.find('\0') != std::string::npos) return false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() > 4096 || values.size() > 512) return false;
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const auto key = trim(line.substr(0, equal));
        if (key.empty()) continue;
        values[key] = trim(line.substr(equal + 1));
    }
    return true;
}

bool executable_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                       static_cast<int>(text.size()), result.data(), length);
    return result;
}

// CreateProcessW takes one command line. This is the documented inverse of the
// Windows C runtime argv parser, including trailing backslashes and quotes.
std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += c;
        } else {
            result.append(slashes, L'\\');
            result += c;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

std::vector<wchar_t> child_environment(const Environment& overrides) {
    std::map<std::wstring, std::wstring> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t* item = block; *item; item += std::wcslen(item) + 1) {
            const std::wstring entry(item);
            const auto equal = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
            if (equal != std::wstring::npos) entries[entry.substr(0, equal)] = entry.substr(equal + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [key, value] : overrides) {
        const auto name = wide(key);
        for (auto it = entries.begin(); it != entries.end();) {
            if (_wcsicmp(it->first.c_str(), name.c_str()) == 0) it = entries.erase(it);
            else ++it;
        }
        entries[name] = wide(value);
    }
    std::vector<wchar_t> result;
    for (const auto& [name, value] : entries) {
        result.insert(result.end(), name.begin(), name.end());
        result.push_back(L'=');
        result.insert(result.end(), value.begin(), value.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}
#endif

// Every child gets a process group / kill-on-close Job. Commands never run
// through cmd.exe, PowerShell, /bin/sh, or a PATH-resolved executable.
class Child {
public:
    ~Child() { terminate(); join(); close_handles(); }
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    bool start(const fs::path& executable, const std::vector<std::string>& arguments,
               const Environment& overrides, const fs::path& log, std::string& error) {
        if (running_) { error = "Process is already running."; return false; }
        for (const auto& arg : arguments) {
            if (arg.size() > 32768 || arg.find('\0') != std::string::npos) {
                error = "Invalid process argument."; return false;
            }
        }
        log_path_ = log;
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&output_pipe_, &write_pipe, &security, 0)) {
            error = "Could not create the child output pipe."; return false;
        }
        SetHandleInformation(output_pipe_, HANDLE_FLAG_INHERIT, 0);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, 0, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            CloseHandle(write_pipe); close_handles(); error = "Could not open child standard input."; return false;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(input); CloseHandle(write_pipe); close_handles();
            error = "Could not create an isolated process job."; return false;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = write_pipe;
        startup.StartupInfo.hStdError = write_pipe;
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attributes(attribute_size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        HANDLE inherited[] = {input, write_pipe};
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited, sizeof(inherited), nullptr, nullptr)) {
            CloseHandle(input); CloseHandle(write_pipe); close_handles();
            error = "Could not limit inherited child handles."; return false;
        }
        std::wstring command = quote_argument(executable.wstring());
        for (const auto& arg : arguments) command += L" " + quote_argument(wide(arg));
        auto env = child_environment(overrides);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            env.data(), executable.parent_path().c_str(), &startup.StartupInfo, &process);
        const DWORD launch_error = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(input);
        CloseHandle(write_pipe);
        if (!created) {
            close_handles(); error = "CreateProcessW failed (Windows error " + std::to_string(launch_error) + ")."; return false;
        }
        process_ = process.hProcess;
        process_id_ = process.dwProcessId;
        if (!AssignProcessToJobObject(job_, process_) || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process_, 1);
            CloseHandle(process.hThread);
            WaitForSingleObject(process_, 1000);
            close_handles(); error = "Could not contain the launched process in its own job."; return false;
        }
        CloseHandle(process.hThread);
#else
        int pipes[2]{};
        if (pipe2(pipes, O_CLOEXEC) != 0) { error = "Could not create child output pipe."; return false; }
        std::vector<std::string> owned_arguments{path_text(executable)};
        owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& arg : owned_arguments) argv.push_back(arg.data());
        argv.push_back(nullptr);
        std::map<std::string, std::string> inherited;
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string value(*entry);
            const auto equal = value.find('=');
            if (equal != std::string::npos) inherited[value.substr(0, equal)] = value.substr(equal + 1);
        }
        for (const auto& [key, value] : overrides) inherited[key] = value;
        std::vector<std::string> owned_environment;
        for (const auto& [key, value] : inherited) owned_environment.push_back(key + "=" + value);
        std::vector<char*> env;
        for (auto& entry : owned_environment) env.push_back(entry.data());
        env.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipes[0]);
        posix_spawn_file_actions_addclose(&actions, pipes[1]);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        pid_t pid = -1;
        const int result = posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv.data(), env.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(pipes[1]);
        if (result != 0) { close(pipes[0]); error = "Could not execute tool (errno " + std::to_string(result) + ")."; return false; }
        output_pipe_ = pipes[0];
        fcntl(output_pipe_, F_SETFL, fcntl(output_pipe_, F_GETFL) | O_NONBLOCK);
        process_id_ = static_cast<std::uint32_t>(pid);
#endif
        running_ = true;
        reader_ = std::thread([this] { collect(); });
        return true;
    }

    bool running() const { return running_; }
    int exit_code() const { return exit_code_; }
    std::uint32_t pid() const { return process_id_; }
    std::string output() const { std::lock_guard lock(output_mutex_); return output_; }

    bool wait(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (running_ && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!running_) { join(); return true; }
        return false;
    }

    void terminate() {
        if (!running_) return;
#ifdef _WIN32
        if (job_) TerminateJobObject(job_, 1);
#else
        if (process_id_) kill(-static_cast<pid_t>(process_id_), SIGKILL);
#endif
    }

    bool owns(std::uint32_t pid) const {
        if (!running_ || !pid) return false;
#ifdef _WIN32
        HANDLE target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!target) return false;
        BOOL contained = FALSE;
        const BOOL checked = IsProcessInJob(target, job_, &contained);
        CloseHandle(target);
        return checked && contained;
#else
        return getpgid(static_cast<pid_t>(pid)) == static_cast<pid_t>(process_id_);
#endif
    }

private:
    std::uint32_t process_id_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> exit_code_{-1};
    std::thread reader_;
    mutable std::mutex output_mutex_;
    std::string output_;
    fs::path log_path_;
#ifdef _WIN32
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE output_pipe_ = nullptr;
#else
    int output_pipe_ = -1;
#endif

    void join() { if (reader_.joinable()) reader_.join(); }
    void close_handles() {
#ifdef _WIN32
        if (output_pipe_) CloseHandle(output_pipe_);
        if (process_) CloseHandle(process_);
        if (job_) CloseHandle(job_);
        output_pipe_ = process_ = job_ = nullptr;
#else
        if (output_pipe_ >= 0) close(output_pipe_);
        output_pipe_ = -1;
#endif
    }

    void collect() {
        std::ofstream log;
        if (!log_path_.empty()) log.open(log_path_, std::ios::binary | std::ios::trunc);
        std::size_t logged = 0;
        const auto append = [&](const char* bytes, std::size_t length) {
            {
                std::lock_guard lock(output_mutex_);
                if (output_.size() < maximum_output) output_.append(bytes, std::min(length, maximum_output - output_.size()));
            }
            if (log && logged < maximum_log) {
                const auto count = std::min(length, maximum_log - logged);
                log.write(bytes, static_cast<std::streamsize>(count));
                log.flush();
                logged += count;
                if (logged == maximum_log) log << "\n[Sifa: log size limit reached; further output omitted.]\n";
            }
        };
        std::array<char, 4096> buffer{};
#ifdef _WIN32
        for (;;) {
            DWORD available = 0;
            const BOOL pipe_ok = PeekNamedPipe(output_pipe_, nullptr, 0, nullptr, &available, nullptr);
            if (pipe_ok && available) {
                DWORD read = 0;
                if (ReadFile(output_pipe_, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) && read)
                    append(buffer.data(), read);
                continue;
            }
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
            const bool active = QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr)
                && state.ActiveProcesses != 0;
            if (!active) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        DWORD code = 1;
        GetExitCodeProcess(process_, &code);
        exit_code_ = static_cast<int>(code);
#else
        bool exited = false;
        bool eof = false;
        int status = 0;
        while (!exited || !eof) {
            const ssize_t count = read(output_pipe_, buffer.data(), buffer.size());
            if (count > 0) append(buffer.data(), static_cast<std::size_t>(count));
            else if (count == 0) eof = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) eof = true;
            if (!exited) {
                const auto result = waitpid(static_cast<pid_t>(process_id_), &status, WNOHANG);
                if (result == static_cast<pid_t>(process_id_) || (result < 0 && errno == ECHILD)) exited = true;
            }
            if (count <= 0 && (!exited || !eof)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
#endif
        running_ = false;
    }
};

struct CommandResult { bool ok = false; std::string output; std::string error; };

CommandResult run_command(const fs::path& executable, const std::vector<std::string>& arguments,
                          const Environment& env, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    CommandResult result;
    Child child;
    if (!child.start(executable, arguments, env, {}, result.error)) return result;
    if (!child.wait(timeout)) {
        child.terminate();
        child.wait(std::chrono::seconds(1));
        result.error = "Tool command timed out.";
        return result;
    }
    result.output = child.output();
    result.ok = child.exit_code() == 0;
    if (!result.ok) result.error = "Tool exited with code " + std::to_string(child.exit_code()) + ": " + trim(result.output).substr(0, 2048);
    return result;
}

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { if (socket != invalid_socket) closesocket(socket); }
bool sockets_ready() {
    static const bool ready = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return ready;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { if (socket != invalid_socket) close(socket); }
bool sockets_ready() { return true; }
#endif

struct ReservedPort {
    Socket socket = invalid_socket;
    unsigned port = 0;
    ~ReservedPort() { release(); }
    void release() { close_socket(socket); socket = invalid_socket; }
    bool reserve(unsigned requested) {
        release();
        if (!sockets_ready()) return false;
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == invalid_socket) return false;
#ifdef _WIN32
        BOOL exclusive = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        fcntl(socket, F_SETFD, FD_CLOEXEC);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(requested));
        if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { release(); return false; }
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) { release(); return false; }
        port = ntohs(address.sin_port);
        return true;
    }
};

bool owns_listener(const Child& child, unsigned port) {
    if (!child.running()) return false;
#ifdef _WIN32
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER || size > 16 * 1024 * 1024) return false;
    std::vector<unsigned char> table(size);
    if (GetExtendedTcpTable(table.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
    const auto* rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(table.data());
    for (DWORD index = 0; index < rows->dwNumEntries; ++index) {
        const auto& row = rows->table[index];
        if (ntohs(static_cast<unsigned short>(row.dwLocalPort)) == port &&
            row.dwLocalAddr == htonl(INADDR_LOOPBACK) && child.owns(row.dwOwningPid)) return true;
    }
#else
    std::set<std::string> inodes;
    std::ifstream tcp("/proc/net/tcp");
    std::string line;
    while (std::getline(tcp, line)) {
        std::istringstream fields(line);
        std::vector<std::string> values;
        std::string value;
        while (fields >> value) values.push_back(value);
        if (values.size() < 10 || values[3] != "0A") continue;
        const auto colon = values[1].find(':');
        if (colon == std::string::npos || values[1].substr(0, colon) != "0100007F") continue;
        unsigned listener_port = 0;
        const auto text = values[1].substr(colon + 1);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), listener_port, 16);
        if (parsed.ec == std::errc{} && listener_port == port) inodes.insert("socket:[" + values[9] + "]");
    }
    if (inodes.empty()) return false;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        unsigned pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || !child.owns(pid)) continue;
        std::error_code fd_error;
        for (fs::directory_iterator fd(it->path() / "fd", fd_error), last; !fd_error && fd != last; fd.increment(fd_error)) {
            const auto target = fs::read_symlink(fd->path(), fd_error).string();
            if (!fd_error && inodes.contains(target)) return true;
            fd_error.clear();
        }
    }
#endif
    return false;
}

fs::path instance_directory(const fs::path& parent, std::string& error) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) { error = "Cannot create Android runtime log directory: " + ec.message(); return {}; }
    std::random_device random;
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::ostringstream name;
        name << "android-" << std::hex << random() << random();
        const auto candidate = parent / name.str();
        if (!fs::create_directory(candidate, ec)) {
            if (ec) { error = "Cannot create private Android runtime directory: " + ec.message(); return {}; }
            continue;
        }
#ifndef _WIN32
        fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (ec) { error = "Cannot protect Android runtime directory permissions."; return {}; }
#endif
        fs::create_directory(candidate / ".android", ec);
        if (ec) { error = "Cannot create Android tool settings directory."; return {}; }
        return fs::absolute(candidate, ec);
    }
    error = "Cannot allocate a unique Android runtime directory.";
    return {};
}

} // namespace

Sdk discover_sdk(const fs::path& preferred) {
    std::vector<fs::path> candidates;
    if (!preferred.empty()) candidates.push_back(preferred);
    else {
        for (const char* key : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
            const auto value = environment(key);
            if (!value.empty()) candidates.push_back(utf8_path(value));
        }
#ifdef _WIN32
        const auto local = environment("LOCALAPPDATA");
        if (!local.empty()) candidates.push_back(utf8_path(local) / "Android" / "Sdk");
#else
        const auto home = user_home();
        if (!home.empty()) candidates.push_back(home / "Android" / "Sdk");
#endif
    }
    Sdk result;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        result.root = fs::weakly_canonical(fs::absolute(candidate, ec), ec);
        if (ec || result.root.empty()) continue;
#ifdef _WIN32
        result.emulator = result.root / "emulator" / "emulator.exe";
        result.adb = result.root / "platform-tools" / "adb.exe";
#else
        result.emulator = result.root / "emulator" / "emulator";
        result.adb = result.root / "platform-tools" / "adb";
#endif
        if (executable_file(result.emulator) && executable_file(result.adb)) {
            result.valid = true;
            return result;
        }
    }
    result.error = "Android SDK with Android Emulator and Android SDK Platform-Tools was not found. Install them with Android Studio SDK Manager, then select the SDK folder.";
    return result;
}

std::vector<Avd> discover_avds(std::string& error) {
    error.clear();
    std::vector<Avd> result;
    const auto registry = avd_home();
    std::error_code ec;
    if (registry.empty() || !fs::is_directory(registry, ec)) {
        error = "No Android virtual devices found. Create an x86_64 Google Play device in Android Studio Device Manager.";
        return result;
    }
    unsigned visited = 0;
    for (fs::directory_iterator it(registry, ec), end; !ec && it != end; it.increment(ec)) {
        if (++visited > 2048) { error = "Too many entries in the AVD registry."; return {}; }
        if (it->path().extension() != ".ini") continue;
        Avd avd;
        avd.name = path_text(it->path().stem());
        if (!valid_avd_name(avd.name)) continue;
        std::map<std::string, std::string> locator;
        if (!read_ini(it->path(), locator)) continue;
        fs::path directory;
        if (const auto path = locator.find("path"); path != locator.end() && !path->second.empty()) {
            directory = utf8_path(path->second);
            if (!directory.is_absolute()) continue;
        } else directory = registry / (avd.name + ".avd");
        avd.config_path = directory / "config.ini";
        std::map<std::string, std::string> config;
        if (!read_ini(avd.config_path, config)) continue;
        avd.abi = config["abi.type"];
        avd.image_directory = config["image.sysdir.1"];
        avd.google_play = config["PlayStore.enabled"] == "true" || config["tag.id"] == "google_apis_playstore";
        result.push_back(std::move(avd));
    }
    if (ec) error = "Could not read Android virtual devices: " + ec.message();
    else if (result.empty()) error = "No valid Android virtual devices found. Create an x86_64 Google Play device in Android Studio Device Manager.";
    std::sort(result.begin(), result.end(), [](const Avd& a, const Avd& b) { return a.name < b.name; });
    return result;
}

struct Runtime::Impl {
    explicit Impl(fs::path directory) : logs(std::move(directory)) {}
    mutable std::mutex mutex;
    mutable std::mutex state_mutex;
    fs::path logs;
    Sdk sdk;
    Environment env;
    std::shared_ptr<Child> emulator;
    std::shared_ptr<Child> adb_server;
    Status state;
    unsigned console_port = 0;
    unsigned server_port = 0;
    std::uint64_t generation = 0;
    Status published;
    std::shared_ptr<Child> published_emulator;
    InputEndpoint published_endpoint;

    void publish() {
        std::lock_guard lock(state_mutex);
        published = state;
        published_emulator = emulator;
        published_endpoint = {static_cast<std::uint16_t>(console_port), generation};
    }

    struct PublishOnExit {
        Impl& impl;
        ~PublishOnExit() { impl.publish(); }
    };

    void update_exit() {
        state.running = emulator && emulator->running();
        if (!state.running && emulator) {
            state.exit_code = emulator->exit_code();
            state.boot_completed = state.play_store_installed = state.game_installed = false;
            if (state.exit_code != 0 && state.error.empty())
                state.error = "Android Emulator exited with code " + std::to_string(state.exit_code) + ". See the emulator log: " + path_text(state.log_path);
        }
    }

    bool ownership(std::string& error) {
        update_exit();
        if (!state.running) { error = "The Sifa-managed Android instance is not running."; return false; }
        if (!adb_server || !adb_server->running() || !owns_listener(*adb_server, server_port)) {
            error = "The private ADB server is not ready or its listener is not owned by Sifa."; return false;
        }
        if (!owns_listener(*emulator, console_port)) {
            error = "Waiting for the owned Android Emulator console listener."; return false;
        }
        return true;
    }

    CommandResult adb(const std::vector<std::string>& args, bool require_identity = true) {
        CommandResult result;
        if (!ownership(result.error)) return result;
        const std::vector<std::string> prefix{"-H", "127.0.0.1", "-P", std::to_string(server_port), "-s", state.serial};
        if (require_identity) {
            auto identify = prefix;
            identify.insert(identify.end(), {"emu", "avd", "name"});
            const auto identity = run_command(sdk.adb, identify, env);
            std::istringstream lines(identity.output);
            std::string first;
            std::getline(lines, first);
            if (!identity.ok || trim(first) != state.avd_name) {
                result.error = "Waiting for the selected Android device on its private ADB connection.";
                return result;
            }
        }
        // The identity query itself can take seconds while Android starts.
        // Recheck process ownership before issuing the requested command.
        if (!ownership(result.error)) return result;
        auto command = prefix;
        command.insert(command.end(), args.begin(), args.end());
        return run_command(sdk.adb, command, env);
    }

    bool action(const std::vector<std::string>& args, std::string& error) {
        const auto result = adb(args);
        if (!result.ok) { error = result.error; return false; }
        if (result.output.find("Error:") != std::string::npos || result.output.find("Exception") != std::string::npos ||
            result.output.find("No activities found") != std::string::npos) {
            error = trim(result.output).substr(0, 2048); return false;
        }
        error.clear();
        return true;
    }

    bool stop_locked(std::string& error) {
        error.clear();
        if (emulator && emulator->running()) {
            std::string ownership_error;
            if (ownership(ownership_error)) {
                const auto ignored = adb({"emu", "kill"});
                (void)ignored;
                emulator->wait(std::chrono::seconds(5));
            }
            if (emulator->running()) {
                emulator->terminate();
                if (!emulator->wait(std::chrono::seconds(2))) error = "The owned emulator process did not stop before the deadline.";
            }
        }
        if (adb_server) { adb_server->terminate(); adb_server->wait(std::chrono::seconds(2)); }
        update_exit();
        if (error.empty()) state.error.clear();
        return error.empty();
    }
};

Runtime::Runtime(fs::path log_directory) : impl_(std::make_unique<Impl>(std::move(log_directory))) {}
Runtime::~Runtime() { std::string ignored; stop(ignored); }

bool Runtime::start(const LaunchOptions& options, std::string& error) {
    std::lock_guard lock(impl_->mutex);
    Impl::PublishOnExit publish{*impl_};
    error.clear();
    impl_->update_exit();
    if (impl_->state.running) { error = "An Android instance is already running. Stop it before changing its configuration."; return false; }
    if (!valid_avd_name(options.avd_name)) { error = "Select a valid existing Android virtual device."; return false; }
    if (options.memory_mb < 512 || options.memory_mb > 32768 || options.cores < 1 || options.cores > 16) {
        error = "Android RAM must be 512–32768 MB and CPU count must be 1–16."; return false;
    }
    if (options.gpu != "auto" && options.gpu != "host" && options.gpu != "software") {
        error = "GPU mode must be auto, host, or software."; return false;
    }
    if ((options.width == 0) != (options.height == 0) || (options.width &&
        (options.width < 320 || options.height < 320 || options.width > 4096 || options.height > 4096))) {
        error = "Resolution must be unset or have both dimensions between 320 and 4096."; return false;
    }
    impl_->sdk = discover_sdk(options.sdk_root);
    if (!impl_->sdk.valid) { error = impl_->sdk.error; return false; }
    const auto avds = discover_avds(error);
    const auto selected = std::find_if(avds.begin(), avds.end(), [&](const Avd& avd) { return avd.name == options.avd_name; });
    if (selected == avds.end()) { if (error.empty()) error = "The selected Android virtual device no longer exists."; return false; }
    if (selected->abi != "x86_64" && selected->abi != "x86") {
        error = "Select an x86_64 Android virtual device. ARM guests do not use x86 hardware acceleration."; return false;
    }
    if (!impl_->stop_locked(error)) return false;
    impl_->emulator.reset();
    impl_->adb_server.reset();
    const auto directory = instance_directory(impl_->logs, error);
    if (directory.empty()) return false;
    impl_->env = {
        {"ANDROID_HOME", path_text(impl_->sdk.root)},
        {"ANDROID_SDK_ROOT", path_text(impl_->sdk.root)},
        {"ANDROID_AVD_HOME", path_text(fs::absolute(avd_home()))},
        {"ANDROID_USER_HOME", path_text(directory / ".android")},
        {"ANDROID_EMULATOR_HOME", path_text(directory / ".android")},
        {"ANDROID_SDK_HOME", path_text(directory)},
        {"ADB_VENDOR_KEYS", path_text(directory / ".android" / "adbkey")}
    };
    const auto version = run_command(impl_->sdk.emulator, {"-version"}, impl_->env);
    if (!version.ok || version.output.find("Android emulator version") == std::string::npos) {
        error = "The selected SDK emulator could not report an Android Emulator version. " + version.error; return false;
    }
    const auto adb_version = run_command(impl_->sdk.adb, {"version"}, impl_->env);
    if (!adb_version.ok || adb_version.output.find("Android Debug Bridge version") == std::string::npos) {
        error = "The selected SDK adb could not report an Android Debug Bridge version. " + adb_version.error; return false;
    }
    if (!options.software_cpu) {
        const auto acceleration = run_command(impl_->sdk.emulator, {"-accel-check"}, impl_->env);
        if (!acceleration.ok) {
            error = "Hardware acceleration is unavailable. On Windows, enable firmware virtualization and Windows Hypervisor Platform, then restart. " + acceleration.error;
            return false;
        }
    }
    // Recent platform-tools ignore ANDROID_USER_HOME when choosing their own
    // default key, while Emulator honors ANDROID_EMULATOR_HOME. Create an
    // instance key first, then explicitly load that key in the private server.
    // Otherwise starting the server before Emulator creates its key can leave
    // the real Android transport offline. No existing user key is read/copied.
    const auto key_path = directory / ".android" / "adbkey";
    const auto keygen = run_command(impl_->sdk.adb, {"keygen", path_text(key_path)}, impl_->env);
    std::error_code key_error;
    if (!keygen.ok || !fs::is_regular_file(key_path, key_error) || key_error) {
        error = "Could not generate the private Android instance's ADB key. " + keygen.error;
        return false;
    }
    ReservedPort console, adb_device, server;
    bool allocated = false;
    for (unsigned port = 5554; port <= 5584; port += 2) {
        if (console.reserve(port) && adb_device.reserve(port + 1)) { allocated = true; break; }
    }
    if (!allocated || !server.reserve(0)) { error = "No free local ports are available for a private Android instance."; return false; }
    impl_->console_port = console.port;
    impl_->server_port = server.port;
    impl_->env["ANDROID_ADB_SERVER_PORT"] = std::to_string(server.port);
    impl_->env["ADB_SERVER_SOCKET"] = "tcp:127.0.0.1:" + std::to_string(server.port);
    impl_->adb_server = std::make_shared<Child>();
    server.release();
    if (!impl_->adb_server->start(impl_->sdk.adb, {"-L", "tcp:localhost:" + std::to_string(server.port), "server", "nodaemon"},
                                   impl_->env, directory / "adb-server.log", error)) return false;
    const auto server_deadline = Clock::now() + std::chrono::seconds(3);
    while (impl_->adb_server->running() && !owns_listener(*impl_->adb_server, server.port) && Clock::now() < server_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (!impl_->adb_server->running() || !owns_listener(*impl_->adb_server, server.port)) {
        error = "Could not start the private ADB server. " + impl_->adb_server->output().substr(0, 2048);
        impl_->adb_server->terminate(); return false;
    }
    std::vector<std::string> arguments{
        "-avd", options.avd_name, "-port", std::to_string(console.port),
        "-memory", std::to_string(options.memory_mb), "-cores", std::to_string(options.cores),
        "-gpu", options.gpu, "-accel", options.software_cpu ? "off" : "on",
        "-no-snapshot-save", "-no-boot-anim"
    };
    if (options.width) { arguments.push_back("-skin"); arguments.push_back(std::to_string(options.width) + "x" + std::to_string(options.height)); }
    if (options.headless) arguments.insert(arguments.end(), {"-no-window", "-no-audio"});
    console.release();
    adb_device.release();
    impl_->emulator = std::make_shared<Child>();
    impl_->state = {};
    impl_->state.serial = "emulator-" + std::to_string(console.port);
    impl_->state.avd_name = options.avd_name;
    impl_->state.log_path = directory / "emulator.log";
    if (!impl_->emulator->start(impl_->sdk.emulator, arguments, impl_->env, impl_->state.log_path, error)) {
        impl_->adb_server->terminate(); impl_->state.error = error; return false;
    }
    impl_->state.running = true;
    impl_->state.process_id = impl_->emulator->pid();
    ++impl_->generation;
    return true;
}

Status Runtime::refresh() {
    std::lock_guard lock(impl_->mutex);
    Impl::PublishOnExit publish{*impl_};
    impl_->update_exit();
    if (!impl_->state.running) return impl_->state;
    const auto boot = impl_->adb({"shell", "getprop", "sys.boot_completed"});
    if (!boot.ok) {
        impl_->state.error = boot.error;
        impl_->state.boot_completed = impl_->state.play_store_installed = impl_->state.game_installed = false;
        return impl_->state;
    }
    impl_->state.boot_completed = trim(boot.output) == "1";
    impl_->state.error.clear();
    if (!impl_->state.boot_completed) {
        impl_->state.play_store_installed = impl_->state.game_installed = false;
        return impl_->state;
    }
    const auto packages = impl_->adb({"shell", "pm", "list", "packages"});
    impl_->state.play_store_installed = false;
    impl_->state.game_installed = false;
    if (!packages.ok) impl_->state.error = packages.error;
    else {
        std::istringstream lines(packages.output);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(std::move(line));
            if (line == "package:com.android.vending") impl_->state.play_store_installed = true;
            if (line == "package:com.axlebolt.standoff2") impl_->state.game_installed = true;
        }
    }
    impl_->update_exit();
    return impl_->state;
}

Status Runtime::snapshot() const {
    std::lock_guard lock(impl_->state_mutex);
    auto state = impl_->published;
    if (impl_->published_emulator && !impl_->published_emulator->running()) {
        state.running = state.boot_completed = state.play_store_installed = state.game_installed = false;
        state.exit_code = impl_->published_emulator->exit_code();
    }
    return state;
}

bool Runtime::stop(std::string& error) {
    std::lock_guard lock(impl_->mutex);
    Impl::PublishOnExit publish{*impl_};
    return impl_->stop_locked(error);
}

bool Runtime::owned_console_endpoint(InputEndpoint& endpoint, std::string& error) const {
    std::shared_ptr<Child> process;
    {
        std::lock_guard lock(impl_->state_mutex);
        process = impl_->published_emulator;
        endpoint = impl_->published_endpoint;
    }
    if (!process || !process->running() || !owns_listener(*process, endpoint.port)) {
        endpoint = {};
        error = "The running Android console is not owned by this Sifa instance.";
        return false;
    }
    error.clear();
    return true;
}

bool Runtime::owns_process(std::uint32_t process_id) const {
    std::shared_ptr<Child> process;
    {
        std::lock_guard lock(impl_->state_mutex);
        process = impl_->published_emulator;
    }
    return process && process->owns(process_id);
}

bool Runtime::open_play_store(std::string& error) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.boot_completed || !impl_->state.play_store_installed) { error = "Google Play is not available in the running Android device."; return false; }
    return impl_->action({"shell", "am", "start", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-p", "com.android.vending"}, error);
}

bool Runtime::open_game_store_page(std::string& error) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.boot_completed || !impl_->state.play_store_installed) { error = "Google Play is not available in the running Android device."; return false; }
    return impl_->action({"shell", "am", "start", "-a", "android.intent.action.VIEW", "-d", "market://details?id=com.axlebolt.standoff2", "-p", "com.android.vending"}, error);
}

bool Runtime::open_game(std::string& error) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.boot_completed || !impl_->state.game_installed) { error = "Standoff 2 is not installed in the running Android device."; return false; }
    return impl_->action({"shell", "am", "start", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-p", "com.axlebolt.standoff2"}, error);
}

} // namespace sifa::android
