#include "sifa/settings.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-9, message);
}
}

int main() {
    try {
        const sifa::HostInfo strong{"test", "x86_64", 8, 16384};
        auto quality = sifa::settings_for_preset("quality", strong);
        require(quality.width == 1920 && quality.height == 1080, "quality resolution");
        require(quality.cpu_cores == 6 && quality.memory_mb == 6144, "quality resources");

        const sifa::HostInfo weak{"test", "x86_64", 2, 2048};
        auto defaults = sifa::default_settings(weak);
        require(defaults.preset == "performance", "weak host default profile");
        require(defaults.cpu_cores <= 2 && defaults.memory_mb == 1024, "weak host resource headroom");
        require(sifa::default_settings(strong).preset == "balanced", "strong host default profile");

        sifa::Settings excessive;
        excessive.width = 100000;
        excessive.height = -1;
        excessive.target_fps = 0;
        excessive.cpu_cores = 1000;
        excessive.memory_mb = 99999;
        excessive.dpi = -20;
        excessive.sensitivity = std::numeric_limits<double>::quiet_NaN();
        excessive.cm360 = std::numeric_limits<double>::infinity();
        excessive.preset = "invented";
        const auto normalized = sifa::normalize_settings(excessive, strong);
        require(normalized.width == 3840 && normalized.height == 360, "resolution bounds");
        require(normalized.target_fps == 30 && normalized.cpu_cores == 7, "frame/core bounds");
        require(normalized.memory_mb == 14336 && normalized.dpi == 100, "memory and DPI bounds");
        require(normalized.sensitivity == 1 && normalized.cm360 == 35, "nonfinite fallback");
        require(normalized.preset == "balanced", "unknown preset fallback");
        excessive.preset = "custom";
        require(sifa::normalize_settings(excessive, strong).preset == "custom", "custom profile survives normalization");

        for (std::uint64_t ram : {1ull, 127ull, 255ull, 256ull, 511ull, 512ull, 4096ull}) {
            auto s = sifa::normalize_settings(excessive, {"test", "x86_64", 0, ram});
            require(s.memory_mb >= 1 && static_cast<std::uint64_t>(s.memory_mb) <= ram, "low-RAM budget");
            require(s.cpu_cores == 1, "unknown CPU fallback");
        }
        require(sifa::normalize_settings(excessive, {"test", "x86_64", 1, 0}).memory_mb == 1024, "unknown RAM fallback");

        sifa::Settings calibration;
        const double full_turn_counts = calibration.cm360 * calibration.dpi / 2.54;
        near(sifa::mouse_delta_degrees(full_turn_counts, calibration), 360, "cm/360 exact calibration");
        near(sifa::mouse_delta_degrees(-full_turn_counts, calibration), -360, "negative motion");
        near(sifa::mouse_delta_degrees(0, calibration), 0, "zero motion");
        const auto first = sifa::mouse_delta_degrees(12.5, calibration);
        const auto second = sifa::mouse_delta_degrees(-2.25, calibration);
        near(first + second, sifa::mouse_delta_degrees(10.25, calibration), "packet batching invariance");
        const auto old_angle = sifa::mouse_delta_degrees(100, calibration);
        calibration.dpi *= 2;
        near(sifa::mouse_delta_degrees(200, calibration), old_angle, "physical motion DPI invariance");
        calibration.target_fps = 360;
        calibration.width = 3840;
        calibration.height = 2160;
        near(sifa::mouse_delta_degrees(200, calibration), old_angle, "frame and resolution invariance");
        calibration.sensitivity = 2;
        near(sifa::mouse_delta_degrees(200, calibration), old_angle * 2, "sensitivity multiplier");
        bool rejected = false;
        try { sifa::mouse_delta_degrees(std::numeric_limits<double>::infinity(), calibration); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "nonfinite mouse input rejected");
        calibration.dpi = 0;
        rejected = false;
        try { sifa::mouse_delta_degrees(1, calibration); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid calibration rejected");

        const auto host = sifa::detect_host();
        require(!host.os.empty() && !host.arch.empty() && host.logical_cpus >= 1, "host detection");
        std::cout << "settings: profiles, bounds, host detection and mouse calibration passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "settings failure: " << e.what() << '\n';
        return 1;
    }
}
