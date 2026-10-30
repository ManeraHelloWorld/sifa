#include "sifa/game_input.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace sifa::input;
using namespace std::chrono_literals;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { ::close(socket); }
#endif

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Operation operation, const char* message) {
    bool rejected = false;
    try { operation(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, message);
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool write_all(Socket socket, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
#ifdef _WIN32
        const auto count = send(socket, text.data() + offset, static_cast<int>(text.size() - offset), 0);
#else
        const auto count = send(socket, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::string read_line(Socket socket) {
    std::string result;
    for (std::size_t count = 0; count < 16384; ++count) {
        char value = 0;
        if (recv(socket, &value, 1, 0) != 1) return result;
        if (value == '\n') return result;
        result.push_back(value);
    }
    throw std::runtime_error("Test console received an oversized command");
}

void socket_timeout(Socket socket) {
#ifdef _WIN32
    const DWORD timeout = 2500;
#else
    const timeval timeout{2, 500000};
#endif
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

struct SocketPlatform {
#ifdef _WIN32
    SocketPlatform() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Test Winsock initialization");
    }
    ~SocketPlatform() { WSACleanup(); }
#endif
};

class MockConsole {
public:
    explicit MockConsole(std::function<void(Socket)> script) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener_ != invalid_socket, "Test console socket creation");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            close_socket(listener_);
            listener_ = invalid_socket;
            throw std::runtime_error("Test console loopback listen failed");
        }
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        require(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "Test console bound port lookup");
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this, script = std::move(script)] {
            Socket client = invalid_socket;
            try {
                fd_set readable{};
                FD_ZERO(&readable);
                FD_SET(listener_, &readable);
                timeval timeout{3, 0};
#ifdef _WIN32
                const int selected = select(0, &readable, nullptr, nullptr, &timeout);
#else
                const int selected = select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
                require(selected > 0, "Test console connection timed out");
                client = accept(listener_, nullptr, nullptr);
                require(client != invalid_socket, "Test console accept failed");
                socket_timeout(client);
                script(client);
            } catch (const std::exception& error) {
                failure_ = error.what();
            }
            if (client != invalid_socket) close_socket(client);
        });
    }

    ~MockConsole() {
        if (worker_.joinable()) worker_.join();
        if (listener_ != invalid_socket) close_socket(listener_);
    }

    std::uint16_t port() const { return port_; }
    void finish() {
        if (worker_.joinable()) worker_.join();
        if (!failure_.empty()) throw std::runtime_error(failure_);
    }

private:
    Socket listener_ = invalid_socket;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::string failure_;
};

class FixtureDirectory {
public:
    FixtureDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("sifa-input-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("Could not create isolated test directory");
    }
    ~FixtureDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};

std::string banner(const std::filesystem::path& token_path) {
    return "Android Console: Authentication required\r\n"
           "Android Console: you can find your <auth_token> in\r\n'" +
           utf8(token_path) + "'\r\nOK\r\n";
}

constexpr const char* fixture_token = "test-only-token-not-an-emulator-secret";

void authenticate(Socket socket, const std::filesystem::path& token_path, bool fragment = false) {
    const auto greeting = banner(token_path);
    if (fragment) {
        // Real sockets may split a line or the final OK in arbitrary positions.
        const auto split = greeting.size() - 3;
        require(write_all(socket, greeting.substr(0, split)), "Test greeting write");
        std::this_thread::sleep_for(2ms);
        require(write_all(socket, greeting.substr(split)), "Test fragmented greeting write");
    } else {
        require(write_all(socket, greeting), "Test greeting write");
    }
    require(read_line(socket) == std::string("auth ") + fixture_token, "Expected fixture authentication");
    require(write_all(socket, "OK\r"), "Test authentication response write");
    if (fragment) std::this_thread::sleep_for(2ms);
    require(write_all(socket, "\n"), "Test authentication response terminator write");
}

void serializer_tests() {
    const Frame point{{2, 9, 0.25, 0.75, true}};
    const std::array<std::string, 4> expected{
        "event send 3:47:2 3:57:9 3:53:25 3:54:75 3:58:1 0:0:0\n",
        "event send 3:47:2 3:57:9 3:53:75 3:54:75 3:58:1 0:0:0\n",
        "event send 3:47:2 3:57:9 3:53:75 3:54:25 3:58:1 0:0:0\n",
        "event send 3:47:2 3:57:9 3:53:25 3:54:25 3:58:1 0:0:0\n"
    };
    for (unsigned rotation = 0; rotation < 4; ++rotation)
        require(console_frame(point, rotation, 100) == expected[rotation], "Natural-axis rotation transform");
    require(console_frame({{1, 37, 0.3, 0.4, false}}) == "event send 3:47:1 3:57:-1 0:0:0\n",
            "Lift writes tracking ID -1 without new positions");
    require(console_frame({}).empty(), "Empty frame emits no command");
    const auto multiple = console_frame({{0, 1, 0, 1, true}, {2, 3, 1, 0, true}}, 0, 100);
    require(multiple == "event send 3:47:0 3:57:1 3:53:0 3:54:100 3:58:1 3:47:2 3:57:3 3:53:100 3:54:0 3:58:1 0:0:0\n",
            "Multiple contacts share exactly one SYN_REPORT");
    rejects([&] { console_frame(point, 4); }, "Invalid rotation rejected");
    rejects([&] { console_frame(point, 0, 0); }, "Zero axis maximum rejected");
    rejects([&] { console_frame(point, 0, 65536); }, "Oversized axis maximum rejected");
    rejects([] { console_frame({{-1, 1, 0.5, 0.5, true}}); }, "Negative slot rejected");
    rejects([] { console_frame({{10, 1, 0.5, 0.5, true}}); }, "Unsupported slot rejected");
    rejects([] { console_frame({{0, -1, 0.5, 0.5, true}}); }, "Negative tracking ID rejected");
    rejects([] { console_frame({{0, 1, -0.01, 0.5, true}}); }, "Negative coordinate rejected");
    rejects([] { console_frame({{0, 1, 0.5, 1.01, true}}); }, "Outside coordinate rejected");
    rejects([] { console_frame({{0, 1, std::numeric_limits<double>::quiet_NaN(), 0.5, true}}); },
            "Nonfinite coordinate rejected");
    rejects([] { console_frame(Frame(11, {0, 1, 0.5, 0.5, true})); }, "Oversized touch report rejected");
}

void transport_tests() {
    [[maybe_unused]] SocketPlatform platform;
    FixtureDirectory fixture;
    const auto token_path = fixture.path / ".emulator_console_auth_token";
    { std::ofstream file(token_path, std::ios::binary); file << fixture_token << "\r\n"; }

    std::string error;
    ConsoleInput client;
    require(!client.connect(80, error), "Reserved console port rejected");
    require(!client.send({{{0, 1, 0.5, 0.5, true}}}, 0, error), "Disconnected input fails");
    {
        std::vector<std::string> commands;
        MockConsole server([&](Socket socket) {
            authenticate(socket, token_path, true);
            for (int i = 0; i < 3; ++i) {
                commands.push_back(read_line(socket));
                require(write_all(socket, "O"), "Test fragmented acknowledgement write");
                std::this_thread::sleep_for(1ms);
                require(write_all(socket, "K\r\n"), "Test fragmented acknowledgement terminator");
            }
        });
        require(client.connect(server.port(), error), "Token-authenticated fragmented console handshake");
        const Frames ordered{{{1, 1, 0.5, 0.5, true}}, {{1, 1, 0.6, 0.5, true}}};
        require(client.send(ordered, 0, error), "Ordered touch reports acknowledged");
        require(client.send({}, 0, error), "No-op batch succeeds without a command");
        require(client.release_all(error), "Owned contact slots released");
        client.close();
        server.finish();
        require(commands.size() == 3, "A command per ordered frame and release");
        require(commands[0] + '\n' == console_frame(ordered[0]) && commands[1] + '\n' == console_frame(ordered[1]),
                "The transport preserves press/drag report ordering");
        require(commands[2] == "event send 3:47:0 3:57:-1 3:47:1 3:57:-1 3:47:2 3:57:-1 3:47:3 3:57:-1 3:47:4 3:57:-1 3:47:5 3:57:-1 0:0:0",
                "Release affects only Sifa's six slots");
    }
    {
        MockConsole server([&](Socket socket) {
            authenticate(socket, token_path);
            require(!read_line(socket).empty(), "Expected touch command before rejection");
            require(write_all(socket, "KO: test command rejected\r\n"), "Test command rejection write");
        });
        require(client.connect(server.port(), error), "Second authenticated connection");
        require(!client.send({{{0, 1, 0.5, 0.5, true}}}, 0, error), "Console KO propagates as failure");
        require(error.find("KO:") != std::string::npos, "Console KO is distinguishable from success");
        require(!client.send({{{0, 1, 0.5, 0.5, false}}}, 0, error), "Rejected connection is closed");
        server.finish();
    }
    {
        MockConsole server([](Socket socket) {
            require(write_all(socket, "Android Console: type 'help' for commands\r\nOK\r\n"), "Test unauthenticated greeting");
            require(read_line(socket).empty(), "No command sent to unauthenticated console");
        });
        require(!client.connect(server.port(), error), "Unauthenticated console rejected");
        server.finish();
    }
    {
        const auto wrong_name = fixture.path / "some-other-file";
        { std::ofstream file(wrong_name); file << fixture_token; }
        MockConsole server([&](Socket socket) {
            require(write_all(socket, banner(wrong_name)), "Test wrong token path greeting");
            require(read_line(socket).empty(), "No authentication from unrelated filename");
        });
        require(!client.connect(server.port(), error), "Unrelated token-file name rejected");
        server.finish();
    }
    {
        MockConsole server([](Socket socket) {
            require(write_all(socket, banner(std::filesystem::path("relative/.emulator_console_auth_token"))),
                    "Test relative token path greeting");
            require(read_line(socket).empty(), "No authentication from relative path");
        });
        require(!client.connect(server.port(), error), "Relative token-file path rejected");
        server.finish();
    }
    {
        MockConsole server([](Socket socket) { write_all(socket, std::string(20000, 'x') + "\nOK\n"); });
        require(!client.connect(server.port(), error), "Oversized console banner rejected");
        require(error.find("Oversized") != std::string::npos, "Oversized response reported");
        server.finish();
    }
    {
        { std::ofstream file(token_path); file << "first-line\nsecond-line\n"; }
        MockConsole server([&](Socket socket) {
            require(write_all(socket, banner(token_path)), "Test invalid token greeting");
            require(read_line(socket).empty(), "No authentication from multiline token");
        });
        require(!client.connect(server.port(), error), "Multiline token rejected before sending");
        server.finish();
    }
}
} // namespace

int main() {
    try {
        serializer_tests();
        transport_tests();
        std::cout << "game_input: Protocol B serialization, rotation, authenticated loopback fixture, fragmentation, rejection and bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "game_input failure: " << error.what() << '\n';
        return 1;
    }
}
