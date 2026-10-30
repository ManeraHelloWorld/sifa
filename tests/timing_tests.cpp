#include "sifa/timing.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

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
        const std::array samples{4.0, 1.0, 3.0, 2.0};
        const auto stats = sifa::summarize_timings(samples);
        require(stats.samples == 4, "sample count");
        near(stats.mean_ms, 2.5, "mean");
        near(stats.p50_ms, 2.5, "p50 interpolation");
        near(stats.p95_ms, 3.85, "p95 interpolation");
        near(stats.max_ms, 4, "maximum");
        require(samples[0] == 4, "input samples preserved");
        require(sifa::summarize_timings({}).samples == 0, "empty is not a benchmark");
        const auto one = sifa::summarize_timings(std::array{1.25});
        near(one.p95_ms, 1.25, "single sample");
        near(one.mean_ms, 1.25, "single mean");
        const auto large = sifa::summarize_timings(std::array{1e308, 1e308});
        require(std::isfinite(large.mean_ms) && std::isfinite(large.p95_ms), "large finite samples do not overflow");

        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            bool rejected = false;
            try { sifa::summarize_timings(std::array{invalid}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid timing sample rejected");
        }

        using namespace std::chrono_literals;
        using Clock = sifa::FramePacer::Clock;
        const Clock::time_point zero{};
        sifa::FramePacer pacer(100);
        pacer.reset(zero);
        require(pacer.next_deadline(zero) == zero + 10ms, "first deadline");
        require(pacer.next_deadline(zero + 11ms) == zero + 20ms, "pacing independent of wake jitter");
        require(pacer.next_deadline(zero + 105ms) == zero + 110ms, "missed frames skipped");
        require(pacer.next_deadline(zero + 105ms) == zero + 120ms, "no catch-up burst");
        pacer.reset(zero);
        require(pacer.next_deadline(zero + 10ms) == zero + 20ms, "deadline is strictly future");
        pacer.set_target_fps(0);
        require(pacer.target_fps() == 1, "zero FPS normalized");
        pacer.reset(zero);
        require(pacer.next_deadline(zero) == zero + 1s, "one-FPS period");
        pacer.set_target_fps(99999);
        require(pacer.target_fps() == 1000, "high FPS bounded");
        pacer.reset(zero);
        require(pacer.next_deadline(zero) == zero + 1ms, "1000-FPS period");

        std::cout << "timing: distributions and deterministic deadline pacing passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "timing failure: " << e.what() << '\n';
        return 1;
    }
}
