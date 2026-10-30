#include "sifa/timing.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sifa {

TimingStats summarize_timings(std::span<const double> samples) {
    TimingStats stats;
    if (samples.empty()) return stats;
    std::vector<double> ordered;
    ordered.reserve(samples.size());
    double mean = 0;
    for (const double sample : samples) {
        if (!std::isfinite(sample) || sample < 0)
            throw std::invalid_argument("Timing samples must be finite, nonnegative milliseconds");
        ordered.push_back(sample);
        mean += (sample - mean) / static_cast<double>(ordered.size());
    }
    std::sort(ordered.begin(), ordered.end());
    const auto percentile = [&ordered](double fraction) {
        const double index = fraction * static_cast<double>(ordered.size() - 1);
        const auto low = static_cast<std::size_t>(index);
        const auto high = std::min(low + 1, ordered.size() - 1);
        // The subtraction stays nonnegative, avoiding sum overflow for large samples.
        return ordered[low] + (ordered[high] - ordered[low]) * (index - static_cast<double>(low));
    };
    stats.samples = samples.size();
    stats.mean_ms = mean;
    stats.p50_ms = percentile(0.50);
    stats.p95_ms = percentile(0.95);
    stats.max_ms = ordered.back();
    return stats;
}

FramePacer::FramePacer(unsigned target_fps) { set_target_fps(target_fps); }

void FramePacer::set_target_fps(unsigned target_fps) {
    target_fps_ = std::clamp(target_fps, 1u, 1000u);
    period_ = std::chrono::nanoseconds(1'000'000'000 / target_fps_);
    reset();
}

void FramePacer::reset(TimePoint now) { next_ = now + period_; }

FramePacer::TimePoint FramePacer::next_deadline(TimePoint now) {
    if (next_ <= now) {
        const auto missed = (now - next_) / period_ + 1;
        next_ += period_ * missed;
    }
    const auto deadline = next_;
    next_ += period_;
    return deadline;
}

void FramePacer::wait_next() { std::this_thread::sleep_until(next_deadline()); }

} // namespace sifa
