// Inert Android-tool process fixtures for the runtime-management test suite.
// Built as separate helpers at build time; never copied from the test runner.
// These are NOT Android SDK tools and must never be distributed with Sifa.
// Their fixed responses validate process lifecycle/argv/ownership only, not
// Android boot, game compatibility, performance, or antivirus safety.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef SIFA_ANDROID_FIXTURE_EMULATOR
#error "Define SIFA_ANDROID_FIXTURE_EMULATOR explicitly for a test-only helper target"
#endif

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

Socket listener(unsigned port) {
    Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == invalid_socket) return socket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(socket, 4)) {
        close_socket(socket); return invalid_socket;
    }
    return socket;
}

std::string argument(const std::vector<std::string>& args, const std::string& key) {
    const auto it = std::find(args.begin(), args.end(), key);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string{};
}

bool contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

// Explicit inert process fixtures: these are not Android binaries, boot tests,
// game compatibility checks, or antivirus evidence. They exercise real argv,
// child ownership, bounded subprocesses, and management of one private instance.
#if SIFA_ANDROID_FIXTURE_EMULATOR
int fixture_emulator(const std::vector<std::string>& args) {
    if (contains(args, "-version")) { std::cout << "Android emulator version test-fixture\n"; return 0; }
    if (contains(args, "-accel-check")) { std::cout << "test-fixture accelerator\n"; return 0; }
    if (!env("SIFA_FIXTURE_EXIT").empty()) return 7;
    std::ofstream captured(env("SIFA_FIXTURE_ARGUMENTS"), std::ios::binary);
    for (const auto& arg : args) captured << arg << '\n';
    captured.close();
    const auto port = argument(args, "-port");
    if (port.empty()) return 8;
    const auto socket = listener(static_cast<unsigned>(std::stoul(port)));
    if (socket == invalid_socket) return 9;
    std::cout << "Inert emulator fixture running.\n" << std::flush;
    for (;;) {
        const auto client = accept(socket, nullptr, nullptr);
        if (client == invalid_socket) continue;
        char message[16]{};
        const int count = recv(client, message, sizeof(message), 0);
        close_socket(client);
        if (count == 4 && std::string(message, 4) == "STOP") break;
    }
    close_socket(socket);
    return 0;
}

#else
int fixture_adb(const std::vector<std::string>& args) {
    if (args.size() == 1 && args[0] == "version") { std::cout << "Android Debug Bridge version test-fixture\n"; return 0; }
    if (args.size() == 2 && args[0] == "keygen") {
        // This is a machine-readable record with an exact LF contract on every
        // platform; Windows text mode would otherwise translate it to CRLF.
        std::ofstream count(env("SIFA_FIXTURE_KEYGEN_COUNT"), std::ios::app | std::ios::binary);
        count << "keygen\n";
        std::ofstream key(fs::path(args[1]), std::ios::binary);
        key << "INERT TEST FIXTURE: NOT A CRYPTOGRAPHIC KEY\n";
        return key ? 0 : 15;
    }
    if (contains(args, "nodaemon")) {
        if (env("ADB_VENDOR_KEYS").empty() || !fs::is_regular_file(fs::path(env("ADB_VENDOR_KEYS")))) return 16;
        const auto endpoint = argument(args, "-L");
        if (endpoint.empty()) return 10;
        const auto colon = endpoint.rfind(':');
        const auto socket = listener(static_cast<unsigned>(std::stoul(endpoint.substr(colon + 1))));
        if (socket == invalid_socket) return 11;
        for (;;) std::this_thread::sleep_for(50ms);
    }
    if (argument(args, "-H") != "127.0.0.1" || argument(args, "-P").empty() ||
        argument(args, "-P") == "5037" || argument(args, "-s").rfind("emulator-", 0) != 0) return 12;
    std::ofstream commands(env("SIFA_FIXTURE_COMMANDS"), std::ios::app);
    for (const auto& arg : args) commands << arg << '\t';
    commands << '\n';
    commands.close();
    if (contains(args, "emu") && contains(args, "name")) {
        std::cout << (env("SIFA_FIXTURE_WRONG_ID").empty() ? "Device_1" : "Unrelated_Device") << "\nOK\n";
        return 0;
    }
    if (contains(args, "getprop")) { std::cout << "1\n"; return 0; }
    if (contains(args, "packages")) {
        std::cout << "package:com.android.vending\npackage:com.axlebolt.standoff2\npackage:com.example.other\n";
        return 0;
    }
    if (contains(args, "am")) { std::cout << "Starting: Intent test-fixture\n"; return 0; }
    if (contains(args, "kill")) {
        const auto serial = argument(args, "-s");
        const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(std::stoul(serial.substr(9))));
        if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { close_socket(socket); return 13; }
        send(socket, "STOP", 4, 0);
        close_socket(socket);
        std::cout << "OK\n";
        return 0;
    }
    return 14;
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
#endif
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
#if SIFA_ANDROID_FIXTURE_EMULATOR
    const int result = fixture_emulator(arguments);
#else
    const int result = fixture_adb(arguments);
#endif
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
