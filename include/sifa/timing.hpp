#pragma once

#include <chrono>
#include <cstddef>
#include <span>

namespace sifa {

struct TimingStats {
    std::size_t samples = 0;
    double mean_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double max_ms = 0;
};

// Millisecond samples must be finite and nonnegative. Linear interpolation is
// used for percentiles. An empty span produces zero samples, not a benchmark.
TimingStats summarize_timings(std::span<const double> samples);

// Deadline scheduler for a future renderer. It is not currently a game renderer
// or an FPS measurement. No busy spin; sleep_until permits the CPU to sleep.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit FramePacer(unsigned target_fps);
    void set_target_fps(unsigned target_fps);
    unsigned target_fps() const noexcept { return target_fps_; }
    void reset(TimePoint now = Clock::now());
    // Returns the next strictly future deadline, advances the schedule, and
    // skips missed frames without accumulating latency or a catch-up burst.
    TimePoint next_deadline(TimePoint now = Clock::now());
    void wait_next();

private:
    unsigned target_fps_ = 60;
    std::chrono::nanoseconds period_{};
    TimePoint next_{};
};

} // namespace sifa
