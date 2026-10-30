#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sifa::android {

struct Sdk {
    std::filesystem::path root;
    std::filesystem::path emulator;
    std::filesystem::path adb;
    bool valid = false;
    std::string error;
};

struct Avd {
    std::string name;
    std::string abi;
    std::string image_directory;
    std::filesystem::path config_path;
    bool google_play = false;
};

// Discovery reads existing installations only. It neither downloads components
// nor creates an AVD, changes its configuration, or accepts a licence.
Sdk discover_sdk(const std::filesystem::path& preferred = {});
std::vector<Avd> discover_avds(std::string& error);

struct LaunchOptions {
    std::filesystem::path sdk_root;
    std::string avd_name;
    unsigned memory_mb = 2048;
    unsigned cores = 2;
    std::string gpu = "auto"; // auto, host, software
    unsigned width = 0;
    unsigned height = 0;
    // Test/headless environments only. Neither is a gaming performance mode.
    bool headless = false;
    bool software_cpu = false;
};

struct Status {
    bool running = false;
    bool boot_completed = false;
    bool play_store_installed = false;
    bool game_installed = false;
    std::string serial;
    std::string avd_name;
    std::string error;
    std::filesystem::path log_path;
    int exit_code = -1;
    std::uint32_t process_id = 0;
};

struct InputEndpoint {
    std::uint16_t port = 0;
    std::uint64_t generation = 0;
};

// Owns exactly one emulator process tree. Long operations belong on a worker
// thread; snapshot() just reads cached state. The destructor stops this instance.
class Runtime {
public:
    explicit Runtime(std::filesystem::path log_directory);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start(const LaunchOptions& options, std::string& error);
    Status refresh();
    Status snapshot() const;
    bool stop(std::string& error);
    bool open_play_store(std::string& error);
    bool open_game_store_page(std::string& error);
    bool open_game(std::string& error);
    // These methods verify the currently owned process tree; they never expose
    // a console belonging to an independently started emulator.
    bool owned_console_endpoint(InputEndpoint& endpoint, std::string& error) const;
    bool owns_process(std::uint32_t process_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sifa::android
