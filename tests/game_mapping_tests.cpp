#include "sifa/game_mapping.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace sifa::input;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

template<class Operation>
void rejects(Operation operation, const char* message) {
    bool failed = false;
    try { operation(); }
    catch (const std::invalid_argument&) { failed = true; }
    require(failed, message);
}

// A receiver model validates contact lifetimes independently from Mapper's
// internal state. Drag distance ignores lift/re-centering gaps, as a touch
// consumer must do; all frames remain ordered.
struct Receiver {
    std::array<TouchUpdate, 6> live{};
    Point aim_distance{};
    unsigned aim_lifts = 0;
    unsigned aim_starts = 0;

    void accept(const Frames& frames) {
        for (const auto& frame : frames) {
            require(!frame.empty(), "No empty input reports");
            std::array<bool, 6> seen{};
            for (const auto& update : frame) {
                require(update.slot >= 0 && update.slot < 6, "Bounded contact slot");
                require(update.tracking_id > 0 && update.tracking_id <= 65535,
                        "Tracking ID fits the Android touchscreen range");
                require(std::isfinite(update.x) && std::isfinite(update.y) &&
                        update.x >= 0 && update.x <= 1 && update.y >= 0 && update.y <= 1,
                        "Normalized, finite contact location");
                const auto slot = static_cast<std::size_t>(update.slot);
                require(!seen[slot], "A report updates a slot once");
                seen[slot] = true;
                auto& previous = live[slot];
                if (update.down) {
                    if (previous.down) {
                        require(previous.tracking_id == update.tracking_id, "Contact identity preserved while dragging");
                        if (slot == 1) {
                            aim_distance.x += update.x - previous.x;
                            aim_distance.y += update.y - previous.y;
                        }
                    } else {
                        for (const auto& other : live)
                            require(!other.down || other.tracking_id != update.tracking_id,
                                    "Active contacts have distinct tracking IDs");
                        if (slot == 1) ++aim_starts;
                    }
                } else {
                    require(previous.down && previous.tracking_id == update.tracking_id,
                            "Lift matches an active contact");
                    if (slot == 1) ++aim_lifts;
                }
                previous = update;
            }
        }
    }

    unsigned count() const {
        unsigned result = 0;
        for (const auto& contact : live) if (contact.down) ++result;
        return result;
    }
};
} // namespace

int main() {
    try {
        Mapper mapping;
        Receiver receiver;
        const auto config = mapping.config();
        require(mapping.mouse_move(0, 0).empty(), "Zero movement creates no touch");
        require(mapping.release_all().empty(), "Releasing an idle mapper is a no-op");
        auto forward = mapping.set_button(Button::forward, true);
        require(forward.size() == 2, "Joystick presses its center before dragging");
        near(forward[0][0].x, config.joystick_center.x, "Joystick starts at center X");
        near(forward[0][0].y, config.joystick_center.y, "Joystick starts at center Y");
        receiver.accept(forward);
        const double straight_pixels = std::hypot(
            (receiver.live[0].x - config.joystick_center.x) * config.width,
            (receiver.live[0].y - config.joystick_center.y) * config.height);
        receiver.accept(mapping.set_button(Button::right, true));
        const double diagonal_pixels = std::hypot(
            (receiver.live[0].x - config.joystick_center.x) * config.width,
            (receiver.live[0].y - config.joystick_center.y) * config.height);
        near(diagonal_pixels, straight_pixels, "Diagonal joystick radius equals straight radius");
        require(mapping.set_button(Button::right, true).empty(), "Repeated keydown is a no-op");

        receiver.accept(mapping.set_button(Button::fire, true));
        receiver.accept(mapping.mouse_move(17, -9));
        require(receiver.count() == 3 && mapping.active_touch_count() == 3,
                "Movement, aim and fire remain simultaneously active");
        receiver.accept(mapping.set_button(Button::jump, true));
        receiver.accept(mapping.set_button(Button::reload, true));
        receiver.accept(mapping.set_button(Button::crouch, true));
        require(receiver.count() == 6, "All six independently mapped contacts can coexist");
        const auto release = mapping.release_all();
        require(release.size() == 1 && release[0].size() == 6,
                "Focus-loss release lifts all contacts in one report");
        receiver.accept(release);
        require(receiver.count() == 0 && mapping.active_touch_count() == 0, "Release leaves no stuck contacts");
        require(mapping.release_all().empty(), "Second release emits no invalid lifts");
        receiver.accept(mapping.set_button(Button::forward, true));
        require(receiver.count() == 1, "Release also clears held-key state");
        receiver.accept(mapping.set_button(Button::back, true));
        require(receiver.count() == 0, "Opposite directions cancel without a lingering contact");
        receiver.accept(mapping.set_button(Button::back, false));
        require(receiver.count() == 1, "Releasing opposite key restores still-held direction");
        receiver.accept(mapping.release_all());

        Mapper turns;
        Receiver turn_receiver;
        const auto large_motion = turns.mouse_move(1900, -1250);
        turn_receiver.accept(large_motion);
        require(turn_receiver.aim_starts > 2 && turn_receiver.aim_lifts > 1,
                "Large aim motion re-centers through explicit touch lifetimes");
        near(turn_receiver.aim_distance.x, 1900 * config.mouse_gain * config.height / config.width,
             "Horizontal movement is preserved across aim boundaries");
        near(turn_receiver.aim_distance.y, -1250 * config.mouse_gain,
             "Vertical movement is preserved across aim boundaries");
        require(turn_receiver.count() == 1, "Re-centering leaves exactly one aim contact");
        for (const auto& frame : large_motion) for (const auto& update : frame)
            require(update.x >= config.aim_bounds.left - 1e-12 && update.x <= config.aim_bounds.right + 1e-12 &&
                    update.y >= config.aim_bounds.top - 1e-12 && update.y <= config.aim_bounds.bottom + 1e-12,
                    "All aim reports stay inside configured aim bounds");
        turn_receiver.accept(turns.release_all());
        require(turn_receiver.count() == 0, "A large turn can be fully released");

        Mapper packetized;
        Receiver packet_receiver;
        for (unsigned i = 0; i < 190; ++i) packet_receiver.accept(packetized.mouse_move(10, -5));
        near(packet_receiver.aim_distance.x, 1900 * config.mouse_gain * config.height / config.width,
             "Aim displacement is independent of packet subdivision");
        near(packet_receiver.aim_distance.y, -950 * config.mouse_gain,
             "Vertical aim displacement is independent of packet subdivision");

        MappingConfig portrait = config;
        portrait.width = 720;
        portrait.height = 1280;
        portrait.invert_y = true;
        Mapper portrait_mapper(portrait);
        Receiver portrait_receiver;
        portrait_receiver.accept(portrait_mapper.mouse_move(20, 10));
        near(portrait_receiver.aim_distance.x, 20 * portrait.mouse_gain, "Portrait short-axis scaling X");
        near(portrait_receiver.aim_distance.y, -10 * portrait.mouse_gain * portrait.width / portrait.height,
             "Portrait short-axis scaling and vertical inversion");

        rejects([&] { mapping.mouse_move(std::numeric_limits<double>::quiet_NaN(), 0); }, "NaN mouse input rejected");
        rejects([&] { mapping.mouse_move(0, std::numeric_limits<double>::infinity()); }, "Infinite mouse input rejected");
        rejects([&] { mapping.mouse_move(1e100, 0); }, "Excessive input work rejected");
        require(mapping.active_touch_count() == 0, "Rejected input does not create a contact");
        rejects([&] { mapping.set_button(static_cast<Button>(99), true); }, "Unknown button rejected");
        auto bad = config;
        bad.aim_center.x = bad.aim_bounds.right;
        rejects([&] { Mapper rejected(bad); }, "Aim center on boundary rejected");
        bad = config;
        bad.joystick_center.x = 0;
        rejects([&] { Mapper rejected(bad); }, "Offscreen joystick circle rejected");
        bad = config;
        bad.mouse_gain = std::numeric_limits<double>::infinity();
        rejects([&] { Mapper rejected(bad); }, "Infinite gain rejected");
        bad = config;
        bad.width = 0;
        rejects([&] { Mapper rejected(bad); }, "Zero display dimension rejected");

        // Repeated reversals plus six-contact lifetimes exercise independent
        // tracking and ensure focus loss cannot leave a prior key held.
        Mapper sequence;
        Receiver sequence_receiver;
        for (int i = 0; i < 1000; ++i) {
            sequence_receiver.accept(sequence.set_button(Button::forward, (i % 3) != 0));
            sequence_receiver.accept(sequence.set_button(Button::right, (i % 5) != 0));
            sequence_receiver.accept(sequence.set_button(Button::fire, (i % 7) != 0));
            sequence_receiver.accept(sequence.mouse_move(i % 2 ? 27 : -31, i % 3 ? -19 : 11));
            if (i % 11 == 0) sequence_receiver.accept(sequence.release_all());
            require(sequence_receiver.count() == sequence.active_touch_count(), "Receiver agrees on live-contact count");
        }
        sequence_receiver.accept(sequence.release_all());
        require(sequence_receiver.count() == 0, "Long mixed input sequence leaves no stuck contacts");

        // Keep the first contact alive while the tracking-ID counter wraps.
        // Reusing that ID would merge two fingers in a real Protocol B receiver.
        Mapper wrapping;
        Receiver wrap_receiver;
        wrap_receiver.accept(wrapping.mouse_move(1, 0));
        const int held_id = wrap_receiver.live[1].tracking_id;
        int previous_fire_id = 0;
        bool observed_wrap = false;
        for (unsigned cycle = 0; cycle < 70000; ++cycle) {
            wrap_receiver.accept(wrapping.set_button(Button::fire, true));
            const int fire_id = wrap_receiver.live[2].tracking_id;
            require(fire_id != held_id, "Tracking-ID wrap skips the still-held aim contact");
            require(wrap_receiver.live[1].tracking_id == held_id && wrap_receiver.live[1].down,
                    "Held aim contact survives tracking-ID wrap unchanged");
            if (previous_fire_id > fire_id) observed_wrap = true;
            previous_fire_id = fire_id;
            wrap_receiver.accept(wrapping.set_button(Button::fire, false));
        }
        require(observed_wrap, "Test exercised a complete tracking-ID counter wrap");
        require(wrap_receiver.count() == 1 && wrapping.active_touch_count() == 1,
                "Wrapped press cycles retain only the intentionally held contact");
        wrap_receiver.accept(wrapping.release_all());
        require(wrap_receiver.count() == 0, "Wrapped contacts can be fully released");

        std::cout << "game_mapping: simultaneous contacts, diagonal radius, aim continuity, focus release, bounds and tracking-ID wrap passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "game_mapping failure: " << error.what() << '\n';
        return 1;
    }
}
