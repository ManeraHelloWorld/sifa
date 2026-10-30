#include "sifa/game_input.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace sifa::input {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
bool set_nonblock(Socket socket) { u_long one = 1; return ioctlsocket(socket, FIONBIO, &one) == 0; }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { ::close(socket); }
int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS; }
bool set_nonblock(Socket socket) {
    const int flags = fcntl(socket, F_GETFL);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif
using Clock = std::chrono::steady_clock;
bool wait_socket(Socket socket, bool write, Clock::time_point until) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    if (ms <= 0) return false;
#ifdef _WIN32
    fd_set read_set{}, write_set{}, error_set{};
    FD_ZERO(&read_set); FD_ZERO(&write_set); FD_ZERO(&error_set);
    if (write) FD_SET(socket, &write_set); else FD_SET(socket, &read_set);
    FD_SET(socket, &error_set);
    timeval timeout{static_cast<long>(ms / 1000), static_cast<long>((ms % 1000) * 1000)};
    return select(0, &read_set, &write_set, &error_set, &timeout) > 0 && !FD_ISSET(socket, &error_set);
#else
    pollfd descriptor{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(ms, 1000))) > 0
        && !(descriptor.revents & (POLLERR | POLLNVAL));
#endif
}
std::string token_from_banner(const std::string& banner) {
    const auto marker = banner.find("you can find your <auth_token> in");
    if (marker == std::string::npos) throw std::runtime_error("Console did not request token authentication");
    const auto first = banner.find('\'', marker);
    const auto line_end = first == std::string::npos ? first : banner.find('\n', first);
    const auto last = first == std::string::npos ? first : banner.rfind('\'', line_end);
    if (first == std::string::npos || last == std::string::npos || last <= first || last - first > 4096)
        throw std::runtime_error("Console did not provide a valid token-file path");
    const auto name = banner.substr(first + 1, last - first - 1);
    const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    if (!path.is_absolute() || path.filename() != ".emulator_console_auth_token")
        throw std::runtime_error("Unexpected emulator token-file path");
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > 1024) throw std::runtime_error("Cannot read the emulator authentication token");
    std::ifstream file(path, std::ios::binary);
    char buffer[1025];
    file.read(buffer, sizeof(buffer));
    if (file.gcount() <= 0 || file.gcount() > 1024)
        throw std::runtime_error("Invalid emulator authentication token length");
    std::string token(buffer, static_cast<std::size_t>(file.gcount()));
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) token.pop_back();
    if (token.empty() || token.size() > 512 || token.find_first_of("\r\n \t\0", 0, 5) != std::string::npos)
        throw std::runtime_error("Invalid emulator authentication token");
    return token;
}
}

std::string console_frame(const Frame& frame, unsigned rotation, unsigned axis_max) {
    if (rotation > 3 || axis_max < 1 || axis_max > 65535 || frame.size() > 10)
        throw std::invalid_argument("Invalid touchscreen geometry or frame size");
    if (frame.empty()) return {};
    std::ostringstream out;
    out << "event send";
    unsigned slots = 0;
    for (const auto& touch : frame) {
        if (touch.slot < 0 || touch.slot >= 10 || touch.tracking_id < 0 || touch.tracking_id > 65535
            || !std::isfinite(touch.x) || !std::isfinite(touch.y)
            || touch.x < 0 || touch.x > 1 || touch.y < 0 || touch.y > 1)
            throw std::invalid_argument("Invalid touchscreen update");
        if (slots & (1U << static_cast<unsigned>(touch.slot)))
            throw std::invalid_argument("Duplicate touchscreen slot in one report");
        slots |= 1U << static_cast<unsigned>(touch.slot);
        out << " 3:47:" << touch.slot;
        if (!touch.down) {
            out << " 3:57:-1";
            continue;
        }
        double x = touch.x, y = touch.y;
        if (rotation == 1) { x = touch.y; y = 1 - touch.x; }
        if (rotation == 2) { x = 1 - touch.x; y = 1 - touch.y; }
        if (rotation == 3) { x = 1 - touch.y; y = touch.x; }
        out << " 3:57:" << touch.tracking_id
            << " 3:53:" << std::llround(x * axis_max)
            << " 3:54:" << std::llround(y * axis_max)
            << " 3:58:1";
    }
    out << " 0:0:0\n";
    return out.str();
}

struct ConsoleInput::Impl {
    Socket socket = invalid_socket;
    std::string pending;
#ifdef _WIN32
    bool winsock = false;
    Impl() { WSADATA data{}; winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~Impl() { close(); if (winsock) WSACleanup(); }
#else
    ~Impl() { close(); }
#endif
    void close() {
        if (socket != invalid_socket) close_socket(socket);
        socket = invalid_socket;
        pending.clear();
    }
    bool write(std::string_view text, Clock::time_point until, std::string& error) {
        while (!text.empty()) {
            if (!wait_socket(socket, true, until)) { error = "Touch input write timed out"; return false; }
#ifdef _WIN32
            const int count = ::send(socket, text.data(), static_cast<int>(text.size()), 0);
#else
            const auto count = ::send(socket, text.data(), text.size(), MSG_NOSIGNAL);
#endif
            if (count < 0 && would_block(socket_error())) continue;
            if (count <= 0) { error = "Touch input connection closed"; return false; }
            text.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }
    bool reply(std::string& output, Clock::time_point until, std::string& error) {
        output.clear();
        while (true) {
            const auto newline = pending.find('\n');
            if (newline != std::string::npos) {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "OK") return true;
                if (line.rfind("KO", 0) == 0) { error = line; return false; }
                output += line + '\n';
                if (output.size() > 16384) { error = "Oversized emulator console response"; return false; }
                continue;
            }
            if (pending.size() > 16384) { error = "Oversized emulator console response"; return false; }
            if (!wait_socket(socket, false, until)) { error = "Touch input response timed out"; return false; }
            char data[2048];
            const auto count = recv(socket, data, sizeof(data), 0);
            if (count < 0 && would_block(socket_error())) continue;
            if (count <= 0) { error = "Touch input connection closed"; return false; }
            pending.append(data, static_cast<std::size_t>(count));
        }
    }
};

ConsoleInput::ConsoleInput() : impl_(std::make_unique<Impl>()) {}
ConsoleInput::~ConsoleInput() = default;
void ConsoleInput::close() { impl_->close(); }

bool ConsoleInput::connect(std::uint16_t port, std::string& error) {
    close();
    if (port < 1024) { error = "Invalid emulator console port"; return false; }
#ifdef _WIN32
    if (!impl_->winsock) { error = "Windows sockets are unavailable"; return false; }
#endif
    impl_->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->socket == invalid_socket || !set_nonblock(impl_->socket)) {
        error = "Could not create the touch input connection"; close(); return false;
    }
    int one = 1;
    setsockopt(impl_->socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto until = Clock::now() + std::chrono::seconds(2);
    const int connected = ::connect(impl_->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected != 0 && (!would_block(socket_error()) || !wait_socket(impl_->socket, true, until))) {
        error = "Cannot connect to the owned emulator console"; close(); return false;
    }
    std::string banner;
    if (!impl_->reply(banner, until, error)) { close(); return false; }
    try {
        const auto token = token_from_banner(banner);
        if (!impl_->write("auth " + token + "\n", until, error)
            || !impl_->reply(banner, until, error)) { close(); return false; }
    } catch (const std::exception& e) { error = e.what(); close(); return false; }
    return true;
}

bool ConsoleInput::send(const Frames& frames, unsigned rotation, std::string& error) {
    if (impl_->socket == invalid_socket) { error = "Touch input is disconnected"; return false; }
    if (frames.size() > 1024) { error = "Touch input batch is too large"; return false; }
    const auto until = Clock::now() + std::chrono::milliseconds(500);
    for (const auto& frame : frames) {
        const auto command = console_frame(frame, rotation);
        if (command.empty()) continue;
        std::string response;
        if (!impl_->write(command, until, error) || !impl_->reply(response, until, error)) {
            close(); return false;
        }
    }
    return true;
}

bool ConsoleInput::release_all(std::string& error) {
    Frame release;
    // Sifa owns slots 0..5. Do not alter other slots used by another input path.
    for (int slot = 0; slot < 6; ++slot) release.push_back({slot, 0, 0, 0, false});
    return send({release}, 0, error);
}
}
