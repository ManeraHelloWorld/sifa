#include "sifa/game_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace sifa::input {
namespace {
constexpr double epsilon = 1e-12;

bool valid_coordinate(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

void check_point(Point point, const char* name) {
    if (!valid_coordinate(point.x) || !valid_coordinate(point.y))
        throw std::invalid_argument(std::string(name) + " must be finite normalized coordinates in [0, 1]");
}

bool same_point(Point a, Point b) {
    return std::abs(a.x - b.x) < epsilon && std::abs(a.y - b.y) < epsilon;
}
} // namespace

Mapper::Mapper(MappingConfig config) : config_(config) {
    if (config.width == 0 || config.height == 0 || config.width > 16384 || config.height > 16384)
        throw std::invalid_argument("Display width/height must be in [1, 16384]");
    check_point(config.joystick_center, "Joystick center");
    check_point(config.aim_center, "Aim center");
    check_point(config.fire, "Fire position");
    check_point(config.jump, "Jump position");
    check_point(config.reload, "Reload position");
    check_point(config.crouch, "Crouch position");
    if (!std::isfinite(config.joystick_radius) || config.joystick_radius <= 0 || config.joystick_radius > 0.5)
        throw std::invalid_argument("Joystick radius must be finite and in (0, 0.5]");
    const double shorter = std::min(config.width, config.height);
    const double rx = config.joystick_radius * shorter / config.width;
    const double ry = config.joystick_radius * shorter / config.height;
    if (!valid_coordinate(config.joystick_center.x - rx) ||
        !valid_coordinate(config.joystick_center.x + rx) ||
        !valid_coordinate(config.joystick_center.y - ry) ||
        !valid_coordinate(config.joystick_center.y + ry))
        throw std::invalid_argument("The joystick circle must fit inside the display");
    const auto bounds = config.aim_bounds;
    if (!valid_coordinate(bounds.left) || !valid_coordinate(bounds.right) ||
        !valid_coordinate(bounds.top) || !valid_coordinate(bounds.bottom) ||
        bounds.right - bounds.left < 0.02 || bounds.bottom - bounds.top < 0.02 ||
        config.aim_center.x - bounds.left < 0.005 || bounds.right - config.aim_center.x < 0.005 ||
        config.aim_center.y - bounds.top < 0.005 || bounds.bottom - config.aim_center.y < 0.005)
        throw std::invalid_argument("Aim bounds must contain their center with at least a 0.005 margin and span at least 0.02");
    if (!std::isfinite(config.mouse_gain) || config.mouse_gain <= 0 || config.mouse_gain > 0.1)
        throw std::invalid_argument("Mouse gain must be finite and in (0, 0.1]");
}

int Mapper::next_tracking_id() {
    for (;;) {
        const auto candidate = static_cast<int>(next_tracking_id_);
        if (++next_tracking_id_ >= static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            next_tracking_id_ = 1;
        const auto duplicate = std::any_of(contacts_.begin(), contacts_.end(), [candidate](const Contact& c) {
            return c.down && c.tracking_id == candidate;
        });
        if (!duplicate) return candidate;
    }
}

TouchUpdate Mapper::begin(int slot, Point point) {
    auto& contact = contacts_[static_cast<std::size_t>(slot)];
    contact = {true, next_tracking_id(), point};
    return {slot, contact.tracking_id, point.x, point.y, true};
}

TouchUpdate Mapper::move(int slot, Point point) {
    auto& contact = contacts_[static_cast<std::size_t>(slot)];
    contact.point = point;
    return {slot, contact.tracking_id, point.x, point.y, true};
}

TouchUpdate Mapper::end(int slot) {
    auto& contact = contacts_[static_cast<std::size_t>(slot)];
    const TouchUpdate result{slot, contact.tracking_id, contact.point.x, contact.point.y, false};
    contact.down = false;
    return result;
}

Frames Mapper::set_button(Button button, bool down) {
    const auto index = static_cast<unsigned>(button);
    if (index >= buttons_.size()) throw std::invalid_argument("Unknown mapped button");
    if (buttons_[index] == down) return {};
    buttons_[index] = down;
    Frames result;
    if (index < 4) {
        const int dx = static_cast<int>(buttons_[3]) - static_cast<int>(buttons_[2]);
        const int dy = static_cast<int>(buttons_[1]) - static_cast<int>(buttons_[0]);
        if (dx == 0 && dy == 0) {
            if (contacts_[0].down) result.push_back({end(0)});
            return result;
        }
        if (!contacts_[0].down) result.push_back({begin(0, config_.joystick_center)});
        const double distance = std::hypot(dx, dy);
        const double shorter = std::min(config_.width, config_.height);
        const Point destination{
            config_.joystick_center.x + dx / distance * config_.joystick_radius * shorter / config_.width,
            config_.joystick_center.y + dy / distance * config_.joystick_radius * shorter / config_.height
        };
        if (!same_point(destination, contacts_[0].point)) result.push_back({move(0, destination)});
        return result;
    }
    const int slot = static_cast<int>(index) - 2;
    const std::array<Point, 4> positions{config_.fire, config_.jump, config_.reload, config_.crouch};
    result.push_back({down ? begin(slot, positions[index - 4]) : end(slot)});
    return result;
}

Frames Mapper::mouse_move(double raw_dx, double raw_dy) {
    if (!std::isfinite(raw_dx) || !std::isfinite(raw_dy))
        throw std::invalid_argument("Mouse deltas must be finite");
    const double shorter = std::min(config_.width, config_.height);
    double dx = raw_dx * config_.mouse_gain * shorter / config_.width;
    double dy = raw_dy * config_.mouse_gain * shorter / config_.height * (config_.invert_y ? -1 : 1);
    // Bound each call's work before changing contact state. Corrupt input cannot
    // create an unbounded sequence of re-centering reports. Ordinary mouse
    // motion remains lossless, including every segment crossing an aim edge.
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) > 16.0)
        throw std::invalid_argument("Mouse delta exceeds the bounded input-report budget");
    if (std::abs(dx) < epsilon && std::abs(dy) < epsilon) return {};

    Frames result;
    if (!contacts_[1].down) result.push_back({begin(1, config_.aim_center)});
    const auto bounds = config_.aim_bounds;
    while (std::abs(dx) >= epsilon || std::abs(dy) >= epsilon) {
        const Point current = contacts_[1].point;
        double fraction = 1.0;
        if (dx > 0) fraction = std::min(fraction, (bounds.right - current.x) / dx);
        if (dx < 0) fraction = std::min(fraction, (bounds.left - current.x) / dx);
        if (dy > 0) fraction = std::min(fraction, (bounds.bottom - current.y) / dy);
        if (dy < 0) fraction = std::min(fraction, (bounds.top - current.y) / dy);
        fraction = std::clamp(fraction, 0.0, 1.0);
        if (fraction > epsilon) {
            const Point destination{
                std::clamp(current.x + dx * fraction, bounds.left, bounds.right),
                std::clamp(current.y + dy * fraction, bounds.top, bounds.bottom)
            };
            result.push_back({move(1, destination)});
            dx *= 1.0 - fraction;
            dy *= 1.0 - fraction;
        }
        if (fraction >= 1.0 - epsilon) break;
        result.push_back({end(1)});
        result.push_back({begin(1, config_.aim_center)});
    }
    return result;
}

Frames Mapper::release_all() {
    Frame released;
    for (std::size_t slot = 0; slot < contacts_.size(); ++slot)
        if (contacts_[slot].down) released.push_back(end(static_cast<int>(slot)));
    buttons_.fill(false);
    if (released.empty()) return {};
    return {released};
}

std::size_t Mapper::active_touch_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(contacts_.begin(), contacts_.end(), [](const Contact& c) {
        return c.down;
    }));
}

} // namespace sifa::input
