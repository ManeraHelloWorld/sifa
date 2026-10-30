#include "sifa/server.hpp"
#include "sifa/vm.hpp"
#include "third_party/json.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include "sifa/desktop.hpp"
#endif

int run_android_cli(int argc, char** argv);

int sifa_main(int argc, char** argv) {
    try {
#ifdef _WIN32
        if (argc == 1) return sifa::run_desktop_app();
#endif
        if (argc > 1 && std::string(argv[1]) == "--android")
            return run_android_cli(argc - 2, argv + 2);
        sifa::ServerOptions options;
        options.data_dir = sifa::default_data_dir();
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Sifa 0.2.0 — Android runtime integration preview\n"
                    "Windows: run without arguments for the native desktop launcher.\n"
                    "       sifa --android list [--sdk PATH]\n"
                    "       sifa --android smoke AVD [--sdk PATH] [--headless] [--software-cpu]\n"
                    "       sifa --lab (legacy VM demonstration)\n"
                    "Usage: sifa [--no-open] [--port 0..65535] [--data-dir PATH]\n"
                    "       sifa --diagnostics\n"
                    "       sifa --demo [auto|software|hardware]\n"
                    "Android requires a separately installed SDK and AVD. Game compatibility is unverified.\n";
                return 0;
            }
            if (arg == "--version") { std::cout << "Sifa 0.2.0\n"; return 0; }
            if (arg == "--lab") continue;
            if (arg == "--diagnostics") { std::cout << sifa::diagnostics_json() << '\n'; return 0; }
            if (arg == "--demo") {
                const auto r = sifa::run_guest_demo(i + 1 < argc ? argv[++i] : "auto");
                std::cout << nlohmann::json{{"ok",r.ok},{"backend",r.backend},
                    {"output",r.output},{"error",r.error},{"instructions",r.instructions},
                    {"elapsed_ms",r.elapsed_ms}}.dump(2) << '\n';
                return r.ok ? 0 : 1;
            }
            if (arg == "--no-open") { options.open_browser = false; continue; }
            if ((arg == "--data-dir" || arg == "--port") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--data-dir") { options.data_dir = value; continue; }
                unsigned port = 0;
                const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), port);
                if (ec != std::errc{} || end != value.data()+value.size() || port > 65535)
                    throw std::runtime_error("Port must be an integer between 0 and 65535");
                options.port = static_cast<unsigned short>(port);
                continue;
            }
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
        return sifa::serve(options);
    } catch (const std::exception& error) {
        std::cerr << "Sifa: " << error.what() << '\n';
        return 1;
    }
}

#ifndef _WIN32
int main(int argc, char** argv) { return sifa_main(argc, argv); }
#endif
