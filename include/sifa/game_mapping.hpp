#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sifa::input {

enum class Button { forward, back, left, right, fire, jump, reload, crouch };

struct Point { double x = 0; double y = 0; };
struct Bounds { double left = 0; double top = 0; double right = 1; double bottom = 1; };

// Positions are fractions of the current, already-rotated Android display.
// Radius/gain use the shorter display dimension, so physical screen-space
// diagonals and mouse gain do not depend on aspect ratio. These are adjustable
// starting positions, not a verified layout or sensitivity for any game.
struct MappingConfig {
    Point joystick_center{0.16, 0.76};
    double joystick_radius = 0.10;
    Point aim_center{0.65, 0.50};
    Bounds aim_bounds{0.45, 0.15, 0.95, 0.85};
    Point fire{0.90, 0.48};
    Point jump{0.89, 0.72};
    Point reload{0.77, 0.88};
    Point crouch{0.80, 0.72};
    double mouse_gain = 0.001;
    bool invert_y = false;
    unsigned width = 1280;
    unsigned height = 720;
};

struct TouchUpdate {
    int slot = 0;
    int tracking_id = 0;
    double x = 0;
    double y = 0;
    bool down = false;
};

// Each Frame is one input report. Separate frames preserve initial touch-down,
// dragging, and lift/re-centering order; a transport must not collapse them.
using Frame = std::vector<TouchUpdate>;
using Frames = std::vector<Frame>;

class Mapper {
public:
    explicit Mapper(MappingConfig config = {});

    Frames set_button(Button button, bool down);
    Frames mouse_move(double raw_dx, double raw_dy);
    Frames release_all();

    const MappingConfig& config() const noexcept { return config_; }
    std::size_t active_touch_count() const noexcept;

private:
    struct Contact { bool down = false; int tracking_id = 0; Point point{}; };
    MappingConfig config_;
    std::array<bool, 8> buttons_{};
    std::array<Contact, 6> contacts_{};
    std::uint32_t next_tracking_id_ = 1;

    TouchUpdate begin(int slot, Point point);
    TouchUpdate move(int slot, Point point);
    TouchUpdate end(int slot);
    int next_tracking_id();
};

} // namespace sifa::input
