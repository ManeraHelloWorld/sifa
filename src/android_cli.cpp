#include "sifa/android_runtime.hpp"
#include "sifa/server.hpp"
#include "third_party/json.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
unsigned number(const std::string& value, unsigned low, unsigned high) {
    unsigned result = 0;
    const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (code != std::errc{} || end != value.data() + value.size() || result < low || result > high)
        throw std::runtime_error("Invalid numeric option: " + value);
    return result;
}
}

int run_android_cli(int argc, char** argv) {
    using namespace sifa::android;
    using json = nlohmann::json;
    if (argc == 0) throw std::runtime_error("Expected --android list or --android smoke AVD");
    const std::string command = argv[0];
    if (command != "list" && command != "smoke")
        throw std::runtime_error("Unknown Android command: " + command);
    LaunchOptions options;
    unsigned timeout = 300;
    int first = 1;
    if (command == "smoke") {
        if (argc < 2) throw std::runtime_error("Expected an existing AVD name after smoke");
        options.avd_name = argv[1];
        first = 2;
    }
    for (int i = first; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--headless") { options.headless = true; continue; }
        if (flag == "--software-cpu") { options.software_cpu = true; continue; }
        if (i + 1 >= argc) throw std::runtime_error("Missing value after " + flag);
        const std::string value = argv[++i];
        if (flag == "--sdk") options.sdk_root = std::filesystem::path(
            reinterpret_cast<const char8_t*>(value.c_str()));
        else if (flag == "--memory") options.memory_mb = number(value, 1024, 16384);
        else if (flag == "--cores") options.cores = number(value, 1, 16);
        else if (flag == "--width") options.width = number(value, 320, 4096);
        else if (flag == "--height") options.height = number(value, 320, 4096);
        else if (flag == "--gpu") options.gpu = value;
        else if (flag == "--boot-timeout") timeout = number(value, 10, 1200);
        else throw std::runtime_error("Unknown Android option: " + flag);
    }
    if (command == "list") {
        const auto sdk = discover_sdk(options.sdk_root);
        std::string error;
        auto avds = json::array();
        for (const auto& avd : discover_avds(error))
            avds.push_back({{"name", avd.name}, {"abi", avd.abi},
                {"google_play", avd.google_play}, {"config_path", avd.config_path.string()}});
        std::cout << json{{"sdk_valid", sdk.valid}, {"sdk_root", sdk.root.string()},
            {"sdk_error", sdk.error}, {"avd_error", error}, {"avds", avds}}.dump(2) << '\n';
        return sdk.valid ? 0 : 1;
    }
    Runtime runtime(sifa::default_data_dir() / "android");
    std::string error;
    if (!runtime.start(options, error)) {
        std::cout << json{{"ok", false}, {"stage", "start"}, {"error", error}}.dump(2) << '\n';
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    auto status = runtime.snapshot();
    while (std::chrono::steady_clock::now() < deadline) {
        status = runtime.refresh();
        if (!status.running || status.boot_completed) break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    const bool booted = status.running && status.boot_completed;
    std::string stop_error;
    const bool stopped = runtime.stop(stop_error);
    std::cout << json{{"ok", booted && stopped}, {"boot_completed", status.boot_completed},
        {"play_store_installed", status.play_store_installed}, {"game_installed", status.game_installed},
        {"serial", status.serial}, {"log_path", status.log_path.string()},
        {"error", booted ? status.error : (status.error.empty() ? "Android boot timeout" : status.error)},
        {"stopped", stopped}, {"stop_error", stop_error},
        {"performance_test", false}}.dump(2) << '\n';
    return booted && stopped ? 0 : 1;
}
