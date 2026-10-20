#pragma once

// Keyboard/joystick controls for the shipped Gunmetal 1.36 layouts.
//
// Two independent readers:
//   * `KEYCFG/*.KEY` — 0x178-byte profiles: `char name[16]` followed by 45
//     `{u32 a; u32 b}` pairs.  The game uses the first 31 pairs, one per action
//     in the proven 31-entry action table (Move Forward … Linear Right);
//     `b` is zero in every shipped file but is preserved verbatim.
//   * `GUNMETAL.CFG` — the 0x2000-byte config: the two profile names at 0x1E0
//     (PERFECT/SPLIT1 in the supply), the three joystick axis presets at
//     0x24C, the three sensitivity values in milliseconds at 0x258, the four
//     joystick buttons at 0x264 and the four 0x18C-byte controller records at
//     0x27C.
//
// The key codes are *not* DirectInput DIK values: they index the game's own
// 123-entry name table (0x4a5b88, 6 bytes per entry).  Codes 0x11/0x1F are
// W/S, 0x39 SPACE, 0x6D/0x6F/0x70/0x72 the arrow keys UP/LEFT/RIGHT/DOWN.
// Unknown codes (123..0x7FFFFFFF) are kept and reported, not guessed.
//
// No SDL, no globals, no I/O in `update`: the caller feeds raw key/button/axis
// state and gets per-action values plus edge flags.  Deadzone and diagonal
// normalisation are pure functions of that state.
//
// Format and provenance: docs/CONTROLS_CPP.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// Number of actions in a `.key` profile (the loader reads exactly 31 pairs).
inline constexpr int kActionCount = 31;
/// Pairs physically stored in a `.key` file (31 used, 14 stack tail).
inline constexpr int kKeyPairCount = 45;
/// `.key` size: `name[16]` + 45 * 8.
inline constexpr int kKeyFileSize = 0x178;
/// `GUNMETAL.CFG` size.
inline constexpr int kConfigSize = 0x2000;

/// One source bound to one action.  Exactly one of `key`/`axis`/`button` is
/// significant (>= 0); `invert` flips an axis.  `key == -1` means "no key".
struct Binding {
    std::string action;
    int key = -1;
    int axis = -1;
    int button = -1;
    bool invert = false;
};

/// What to do when a source is already bound to another action.
enum class ConflictPolicy {
    reject, ///< return false and leave the bindings untouched
    allow,  ///< add it; `Controls::conflicts()` reports it
};

/// Per-action state produced by `update`.
struct ActionState {
    double value = 0.0;     ///< [-1, 1]; digital sources are 0 or 1
    bool pressed = false;   ///< rising edge at this update
    bool held = false;      ///< value >= 0.5 after this update
    bool released = false;  ///< falling edge at this update
    double held_time = 0.0; ///< seconds held, accumulated with `dt`
};

/// 2D intent vector: `x` = forward/back, `y` = strafe right/left (or the
/// look axes for `look_vector()`).
struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

/// Raw pair from a `.key` file.
struct KeyPair {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

/// A parsed `.key` profile.
struct KeyProfile {
    std::string name; ///< file `name[16]`, trimmed at the first NUL
    std::array<KeyPair, kKeyPairCount> pairs{};
    std::array<Binding, kActionCount> bindings{};
    std::size_t bytes_read = 0; ///< bytes actually supplied
    bool have_tail = false;     ///< bytes beyond 0x178 were ignored
};

struct KeyLocation {
    int slot = 0;
    std::uint32_t code = 0;
};

struct AxisMask {
    std::uint32_t mask_a = 0;
    std::uint32_t mask_b = 0;
    std::uint32_t preset = 0;
};

struct BitBinding {
    std::uint32_t bit = 0;
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
};

/// One of the four 0x18C-byte controller records at 0x27C.
struct ControllerRecord {
    std::uint32_t slot = 0;
    std::array<std::uint32_t, 32> buttons{};
    std::array<AxisMask, 6> axes{};
    std::array<BitBinding, 16> bits{};
};

/// Parsed `GUNMETAL.CFG` (only the controller-relevant fields are interpreted;
/// the whole 0x2000-byte image is kept in `raw`).
struct ConfigProfile {
    std::array<std::string, 2> key_names;
    std::uint32_t joystick_used_by = 0;
    std::array<std::uint32_t, 3> axis_preset{};   ///< 0x24C: L/R, F/B, U/D
    std::array<std::uint32_t, 3> sensitivity_ms{}; ///< 0x258..0x260
    std::array<std::uint32_t, 4> buttons{};        ///< 0x264: action indices
    std::array<ControllerRecord, 4> records{};
    std::vector<std::uint8_t> raw;

    /// Seconds, with the loader's proven 0.2 s floor (`0x485272`).
    std::array<double, 3> sensitivity_seconds() const;
};

/// Controller state, bindings and edge detection.  Copyable, no globals.
class Controls {
public:
    /// Shipped PERFECT layout (slots 0..28 bound, 29/30 unbound) and a 0.2
    /// deadzone.  The deadzone is application policy, not a config field.
    Controls();

    // --- loaders ---------------------------------------------------------
    /// Parse a `.key` profile.  A file shorter than 0x178 is a clean error; a
    /// longer one is read up to 0x178 and flagged with `profile().have_tail`.
    /// Unknown key codes (123..0x7FFFFFFF) are preserved; a code with the top
    /// bit set is rejected as corrupt.  On error nothing changes.
    bool load_key_file(const std::filesystem::path& path, std::string* error = nullptr);

    /// Parse `GUNMETAL.CFG` (exact 0x25-byte magic `av1.93`).  Applies the
    /// four joystick buttons and the three axis presets as bindings.
    bool load_config(const std::filesystem::path& path, std::string* error = nullptr);

    // --- bindings --------------------------------------------------------
    /// Replace the same source kind, or add it.  A key replaces the action's
    /// keyboard binding; a button replaces only the same button index (so two
    /// buttons can share an action); an axis replaces only the same axis and
    /// `invert` (so an opposite-signed axis pair coexists).  `reject` refuses
    /// a key/button/axis sign already owned by another action.  Unknown
    /// actions and malformed sources are errors.
    bool bind(const Binding& binding, ConflictPolicy policy = ConflictPolicy::reject,
              std::string* error = nullptr);

    /// Sources bound to more than one action, as `key 17: forward/fire`.
    std::vector<std::string> conflicts() const;

    // --- raw input (caller-owned, no platform code) ----------------------
    void set_key(int code, bool down);
    void set_button(int index, bool down);
    /// Store a raw axis value.  Non-finite values are rejected by `update`.
    void set_axis(int index, double value);

    /// Validate `dt` and the raw axes, recompute every action, then advance the
    /// edge flags.  On error the previous state is kept untouched.
    bool update(double dt, std::string* error = nullptr);

    // --- queries ---------------------------------------------------------
    std::optional<std::string> map_key(int code) const;
    std::optional<std::string> map_button(int index) const;

    struct AxisSample {
        std::string action;
        double value = 0.0;
    };
    /// First binding for `index`, with `invert` and the deadzone applied.
    std::optional<AxisSample> map_axis(int index, double raw) const;

    const ActionState& action(std::string_view name) const;
    bool has_action(std::string_view name) const;
    bool pressed(std::string_view name) const;
    bool held(std::string_view name) const;
    bool released(std::string_view name) const;
    double value(std::string_view name) const;

    /// Digital/analog movement intent, scaled so its length never exceeds 1.
    Vec2 move_vector() const;
    /// Turn/look intent, same normalisation.
    Vec2 look_vector() const;

    // --- accessors -------------------------------------------------------
    const KeyProfile& profile() const { return profile_; }
    const ConfigProfile& config() const { return config_; }
    const std::vector<Binding>& bindings() const { return bindings_; }
    const std::vector<KeyLocation>& unknown_keys() const { return unknown_keys_; }
    const std::string& last_error() const { return last_error_; }

    double deadzone() const { return deadzone_; }
    /// Valid range [0, 1]; 1 silences every analog axis.
    bool set_deadzone(double deadzone, std::string* error = nullptr);

    // --- proven tables ---------------------------------------------------
    /// Canonical action name for slot 0..30 ("" outside).
    static std::string_view action_name(int slot);
    /// Slot for a canonical name (or a documented alias), -1 if unknown.
    static int action_slot(std::string_view name);
    /// Display name from the game's 123-entry table, or `KEY_<n>`.
    static std::string key_name(int code);
    /// True for 0..122 (the game's table).
    static bool valid_key(int code);

private:
    bool fail(std::string* error, std::string message);
    void install_key_profile(const KeyProfile& profile);
    void apply_config_bindings(const ConfigProfile& config);
    double apply_deadzone(double raw) const;
    ActionState* find_action(std::string_view name);
    const ActionState* find_action(std::string_view name) const;

    KeyProfile profile_;
    ConfigProfile config_;
    std::vector<Binding> bindings_;
    std::vector<KeyLocation> unknown_keys_;
    std::vector<ActionState> states_{static_cast<std::size_t>(kActionCount)};
    std::vector<bool> previous_held_ =
        std::vector<bool>(static_cast<std::size_t>(kActionCount), false);
    std::vector<int> key_down_;    ///< raw key code -> 1/0
    std::vector<int> button_down_; ///< raw button -> 1/0
    std::vector<double> axis_raw_; ///< raw axis -> value
    double deadzone_ = 0.2;
    std::string last_error_;
};

} // namespace gm
