#pragma once
#include <filesystem>
#include <string>
namespace sifa {
struct ServerOptions {
    unsigned short port = 0;
    bool open_browser = true;
    std::filesystem::path data_dir;
};
std::filesystem::path default_data_dir();
std::string diagnostics_json();
int serve(const ServerOptions& options);
}
