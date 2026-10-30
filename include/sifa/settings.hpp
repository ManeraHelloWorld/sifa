#pragma once

#include <cstdint>
#include <string>

namespace sifa {

struct HostInfo {
    std::string os;
    std::string arch;
    unsigned logical_cpus = 1;
    // Physical memory, limited by a detected Linux container budget if present.
    // Zero means unknown, rather than an invented measurement.
    std::uint64_t memory_mb = 0;
};

struct Settings {
    std::string preset = "balanced";
    int width = 1600;
    int height = 900;
    int target_fps = 120;
    int cpu_cores = 2;
    int memory_mb = 2048;
    int dpi = 800;
    double sensitivity = 1.0;
    double cm360 = 35.0;
    bool raw_input = true;
};

HostInfo detect_host();
Settings default_settings(const HostInfo& host);
Settings normalize_settings(Settings settings, const HostInfo& host);
// Supported presets: performance, balanced, quality. Unknown names use balanced.
Settings settings_for_preset(const std::string& preset, const HostInfo& host);

// Maps one axis of relative mouse counts directly to degrees. There is no
// acceleration, smoothing, frame-rate dependence or resolution dependence.
// cm360 is exact when sensitivity == 1; effective cm/360 is cm360/sensitivity.
// Throws for nonfinite counts, invalid calibration or an overflowing result.
double mouse_delta_degrees(double counts, const Settings& settings);

} // namespace sifa
