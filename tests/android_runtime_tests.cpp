#include "sifa/android_runtime.hpp"

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

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
std::uint32_t own_pid() { return GetCurrentProcessId(); }
void set_environment(const char* key, const std::string& value) { _putenv_s(key, value.c_str()); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { close(socket); }
std::uint32_t own_pid() { return static_cast<std::uint32_t>(getpid()); }
void set_environment(const char* key, const std::string& value) { setenv(key, value.c_str(), 1); }
#endif

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

struct Environment {
    std::string key;
    std::string previous;
    bool existed;
    Environment(const char* name, const std::string& value)
        : key(name), previous(env(name)), existed(std::getenv(name) != nullptr) { set_environment(name, value); }
    ~Environment() {
#ifndef _WIN32
        if (!existed) { unsetenv(key.c_str()); return; }
#endif
        set_environment(key.c_str(), previous);
    }
};

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

int fixture_adb(const std::vector<std::string>& args) {
    if (args.size() == 1 && args[0] == "version") { std::cout << "Android Debug Bridge version test-fixture\n"; return 0; }
    if (args.size() == 2 && args[0] == "keygen") {
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

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
}

int failures = 0;
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
#endif
    const auto name = fs::path(argv[0]).stem().string();
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    if (name == "emulator") return fixture_emulator(arguments);
    if (name == "adb") return fixture_adb(arguments);

    const auto directory = fs::temp_directory_path() / ("sifa runtime fixtures " + std::to_string(own_pid()));
    fs::create_directories(directory / "sdk space & literal" / "emulator");
    fs::create_directories(directory / "sdk space & literal" / "platform-tools");
    fs::create_directories(directory / "avd" / "Device_1.avd");
    const auto sdk_root = directory / "sdk space & literal";
#ifdef _WIN32
    const auto emulator_path = sdk_root / "emulator" / "emulator.exe";
    const auto adb_path = sdk_root / "platform-tools" / "adb.exe";
#else
    const auto emulator_path = sdk_root / "emulator" / "emulator";
    const auto adb_path = sdk_root / "platform-tools" / "adb";
#endif
    fs::copy_file(fs::absolute(argv[0]), emulator_path, fs::copy_options::overwrite_existing);
    fs::copy_file(fs::absolute(argv[0]), adb_path, fs::copy_options::overwrite_existing);
    fs::permissions(emulator_path, fs::perms::owner_all);
    fs::permissions(adb_path, fs::perms::owner_all);
    const auto avd_config = directory / "avd" / "Device_1.avd" / "config.ini";
    const std::string config = "abi.type=x86_64\r\nPlayStore.enabled=true\r\nimage.sysdir.1=system-images/android-30/google_apis_playstore/x86_64/\r\n";
    write_file(avd_config, config);
    write_file(directory / "avd" / "Device_1.ini", "path=" + (directory / "avd" / "Device_1.avd").string() + "\n");
    write_file(directory / "avd" / "Bad Name.ini", "path=" + (directory / "avd" / "Device_1.avd").string() + "\n");
    write_file(directory / "avd" / "Oversized.ini", std::string(300000, 'x'));
    Environment avd_environment("ANDROID_AVD_HOME", (directory / "avd").string());
    Environment sdk_environment("ANDROID_HOME", sdk_root.string());
    Environment capture_environment("SIFA_FIXTURE_ARGUMENTS", (directory / "arguments.txt").string());
    Environment command_environment("SIFA_FIXTURE_COMMANDS", (directory / "commands.txt").string());

    const auto sdk = sifa::android::discover_sdk(sdk_root);
    check(sdk.valid && sdk.root == fs::weakly_canonical(sdk_root), "SDK discovery preserves executable paths containing spaces and shell punctuation");
    check(sifa::android::discover_sdk().valid, "SDK environment discovery");
    check(!sifa::android::discover_sdk(directory / "missing SDK").valid, "explicit invalid SDK never silently falls back");
    std::string error;
    const auto avds = sifa::android::discover_avds(error);
    check(avds.size() == 1 && avds[0].name == "Device_1" && avds[0].google_play, "safe existing AVD discovery excludes malformed and oversized entries");
    check(!avds.empty() && avds[0].abi == "x86_64", "AVD ABI is read from config");

    {
        sifa::android::Runtime runtime(directory / "private logs");
        sifa::android::LaunchOptions options;
        options.sdk_root = sdk_root;
        options.avd_name = "Device_1";
        options.memory_mb = 1536;
        options.cores = 2;
        options.gpu = "software";
        options.width = 1280;
        options.height = 720;
        options.headless = true;
        options.software_cpu = true;
        auto invalid = options;
        invalid.avd_name = "Device_1; touch injected";
        check(!runtime.start(invalid, error), "AVD command injection is rejected");
        invalid = options; invalid.memory_mb = 0;
        check(!runtime.start(invalid, error), "invalid memory is rejected");
        invalid = options; invalid.gpu = "host -wipe-data";
        check(!runtime.start(invalid, error), "invalid GPU option is rejected");
        invalid = options; invalid.height = 0;
        check(!runtime.start(invalid, error), "partial display size is rejected");
        check(!runtime.open_game(error), "game action fails before Android is running");

        const auto unrelated = listener(5554);
        const bool started = runtime.start(options, error);
        check(started, "real child process fixture launches: " + error);
        if (started) {
            auto status = runtime.snapshot();
            check(status.running && status.process_id != 0, "launch reports actual managed process");
            check(status.serial != "emulator-5554" || unrelated == invalid_socket, "an unrelated listener is preserved and skipped");
            check(!runtime.start(options, error), "cannot start a second instance over an owned live process");
            const auto deadline = std::chrono::steady_clock::now() + 4s;
            do {
                status = runtime.refresh();
                if (status.boot_completed) break;
                std::this_thread::sleep_for(30ms);
            } while (std::chrono::steady_clock::now() < deadline);
            check(status.boot_completed && status.play_store_installed && status.game_installed,
                  "status comes from scoped adb process fixture output: " + status.error);
            check(runtime.owns_process(status.process_id), "owned child PID is recognized");
            check(!runtime.owns_process(own_pid()), "parent and unrelated processes are not owned");
            sifa::android::InputEndpoint endpoint;
            check(runtime.owned_console_endpoint(endpoint, error) && endpoint.port && endpoint.generation,
                  "input endpoint is released only after actual listener ownership validation");
            check(runtime.open_play_store(error), "Play Store launch uses explicit owned device");
            check(runtime.open_game_store_page(error), "game store page uses explicit owned device");
            check(runtime.open_game(error), "installed game launch uses explicit owned device");
            {
                Environment wrong_identity("SIFA_FIXTURE_WRONG_ID", "1");
                status = runtime.refresh();
                check(!status.boot_completed && !status.error.empty(), "a different AVD identity cannot be reported as the selected Android instance");
                check(!runtime.open_game(error), "game actions are refused after an AVD identity mismatch");
            }
            status = runtime.refresh();
            check(status.boot_completed, "the selected instance can recover after a transient identity query failure");
            const auto captured = read_file(directory / "arguments.txt");
            check(captured.find("-memory\n1536\n-cores\n2\n-gpu\nsoftware\n-accel\noff") != std::string::npos,
                  "requested resources and renderer reach the child unchanged");
            check(captured.find("-skin\n1280x720\n") != std::string::npos && captured.find("-wipe-data") == std::string::npos,
                  "display size is passed without destructive image flags");
            check(read_file(avd_config) == config, "AVD configuration is not overwritten");
            check(runtime.stop(error), "owned emulator stops gracefully: " + error);
            check(!runtime.snapshot().running, "stopped process is not reported as running");
            check(!runtime.owned_console_endpoint(endpoint, error), "stopped instance cannot expose input endpoint");
            check(fs::exists(status.log_path), "owned process output has a real log file");
        }
        if (unrelated != invalid_socket) close_socket(unrelated);
    }
    {
        Environment exit_fixture("SIFA_FIXTURE_EXIT", "1");
        sifa::android::Runtime runtime(directory / "failure logs");
        sifa::android::LaunchOptions options;
        options.sdk_root = sdk_root; options.avd_name = "Device_1"; options.software_cpu = true;
        const bool started = runtime.start(options, error);
        check(started, "early-exit fixture starts as a real child");
        std::this_thread::sleep_for(150ms);
        const auto status = runtime.refresh();
        check(!status.running && status.exit_code == 7 && !status.error.empty(), "child failure is surfaced without reporting a successful Android boot");
    }
    fs::remove_all(directory);
    std::cout << checks << " Android runtime management fixture checks, " << failures << " failed.\n"
              << "These tests do not validate Android, Google Play, Standoff 2, game FPS, or antivirus safety.\n";
    return failures ? 1 : 0;
}
