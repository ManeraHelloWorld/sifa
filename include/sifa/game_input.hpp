#pragma once

#include "sifa/game_mapping.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sifa::input {

// The console consumes the touchscreen's natural (unrotated) Linux coordinates.
// rotation is the Android Surface rotation, 0..3, in quarter turns.
std::string console_frame(const Frame& frame, unsigned rotation = 0,
                          unsigned axis_max = 32767);

// Authenticated persistent loopback connection. It never starts adb or a shell.
// The caller must verify that port belongs to its own emulator before connecting.
class ConsoleInput {
public:
    ConsoleInput();
    ~ConsoleInput();
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;
    bool connect(std::uint16_t port, std::string& error);
    bool send(const Frames& frames, unsigned rotation, std::string& error);
    bool release_all(std::string& error);
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace sifa {
namespace android { class Runtime; }
class GameInput {
public:
    GameInput(HWND owner, android::Runtime& runtime,
              std::filesystem::path settings_file);
    ~GameInput();
    GameInput(const GameInput&) = delete;
    GameInput& operator=(const GameInput&) = delete;
    void show_settings();
    bool handle_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
