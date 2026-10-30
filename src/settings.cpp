#include "sifa/settings.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace sifa {
namespace {

unsigned core_budget(const HostInfo& host) {
    const unsigned available = std::max(1u, host.logical_cpus);
    return std::min(16u, available > 2 ? available - 1 : available);
}

int memory_budget(const HostInfo& host) {
    if (host.memory_mb == 0) return 1024; // Conservative fallback, not telemetry.
    const auto reserve = std::min<std::uint64_t>(2048, host.memory_mb / 2);
    const auto budget = std::min<std::uint64_t>(16384, host.memory_mb - reserve);
    return static_cast<int>(std::max<std::uint64_t>(1, budget));
}

double finite_clamp(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

#ifdef __linux__
std::uint64_t positive_file_number(const char* path) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value) || value.empty() || value[0] == '-') return 0;
    try {
        std::size_t consumed = 0;
        const auto number = std::stoull(value, &consumed);
        return consumed == value.size() ? number : 0;
    } catch (...) {
        return 0;
    }
}

void apply_linux_limits(HostInfo& host) {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        const int count = CPU_COUNT(&affinity);
        if (count > 0) host.logical_cpus = std::min(host.logical_cpus, static_cast<unsigned>(count));
    }

    std::ifstream cpu_max("/sys/fs/cgroup/cpu.max");
    std::string quota_text;
    std::uint64_t period = 0;
    std::uint64_t quota = 0;
    if ((cpu_max >> quota_text >> period) && quota_text != "max") {
        try {
            std::size_t consumed = 0;
            quota = std::stoull(quota_text, &consumed);
            if (consumed != quota_text.size() || quota_text[0] == '-') quota = 0;
        } catch (...) { quota = 0; }
    } else {
        quota = positive_file_number("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        period = positive_file_number("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    }
    if (quota && period) {
        const auto quota_cores = quota / period + (quota % period != 0 ? 1 : 0);
        host.logical_cpus = static_cast<unsigned>(std::min<std::uint64_t>(host.logical_cpus, quota_cores));
    }

    for (const char* path : {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"}) {
        const auto bytes = positive_file_number(path);
        if (!bytes) continue;
        const auto mb = std::max<std::uint64_t>(1, bytes / (1024 * 1024));
        if (host.memory_mb == 0 || mb < host.memory_mb) host.memory_mb = mb;
    }
}
#endif

} // namespace

HostInfo detect_host() {
    HostInfo host;
    host.logical_cpus = std::max(1u, std::thread::hardware_concurrency());
#if defined(_WIN32)
    host.os = "Windows";
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) host.memory_mb = status.ullTotalPhys / (1024 * 1024);
#elif defined(__linux__)
    host.os = "Linux";
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        const auto pages64 = static_cast<std::uint64_t>(pages);
        const auto size64 = static_cast<std::uint64_t>(page_size);
        if (pages64 <= std::numeric_limits<std::uint64_t>::max() / size64)
            host.memory_mb = (pages64 * size64) / (1024 * 1024);
    }
    apply_linux_limits(host);
#elif defined(__APPLE__)
    host.os = "macOS";
#else
    host.os = "Unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    host.arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    host.arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    host.arch = "x86";
#else
    host.arch = "unknown";
#endif
    return host;
}

Settings normalize_settings(Settings settings, const HostInfo& host) {
    if (settings.preset != "performance" && settings.preset != "balanced" &&
        settings.preset != "quality" && settings.preset != "custom")
        settings.preset = "balanced";
    settings.width = std::clamp(settings.width, 640, 3840);
    settings.height = std::clamp(settings.height, 360, 2160);
    settings.target_fps = std::clamp(settings.target_fps, 30, 360);
    settings.cpu_cores = std::clamp(settings.cpu_cores, 1, static_cast<int>(core_budget(host)));
    const int max_memory = memory_budget(host);
    settings.memory_mb = std::clamp(settings.memory_mb, std::min(256, max_memory), max_memory);
    settings.dpi = std::clamp(settings.dpi, 100, 32000);
    settings.sensitivity = finite_clamp(settings.sensitivity, 0.05, 10.0, 1.0);
    settings.cm360 = finite_clamp(settings.cm360, 1.0, 200.0, 35.0);
    return settings;
}

Settings settings_for_preset(const std::string& preset, const HostInfo& host) {
    Settings settings;
    settings.preset = preset;
    if (preset == "performance") {
        settings.width = 1280;
        settings.height = 720;
        settings.cpu_cores = 2;
        settings.memory_mb = 2048;
    } else if (preset == "quality") {
        settings.width = 1920;
        settings.height = 1080;
        settings.target_fps = 144;
        settings.cpu_cores = 6;
        settings.memory_mb = 6144;
    } else {
        settings.preset = "balanced";
        settings.cpu_cores = 4;
        settings.memory_mb = 4096;
    }
    return normalize_settings(settings, host);
}

Settings default_settings(const HostInfo& host) {
    const bool limited = host.logical_cpus < 4 || host.memory_mb < 6144;
    return settings_for_preset(limited ? "performance" : "balanced", host);
}

double mouse_delta_degrees(double counts, const Settings& settings) {
    if (!std::isfinite(counts) || settings.dpi <= 0 ||
        !std::isfinite(settings.sensitivity) || settings.sensitivity <= 0 ||
        !std::isfinite(settings.cm360) || settings.cm360 <= 0) {
        throw std::invalid_argument("Mouse calibration and counts must be finite and calibration must be positive");
    }
    const double degrees_per_count = (360.0 * 2.54 / settings.dpi / settings.cm360) * settings.sensitivity;
    const double result = counts * degrees_per_count;
    if (!std::isfinite(result)) throw std::overflow_error("Mouse delta exceeds representable range");
    return result;
}

} // namespace sifa
