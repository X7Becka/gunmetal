// `.key` / `GUNMETAL.CFG` controller parsing and deterministic input state.
//
// Proven layout (docs/CONFIG.md §6, §9 and docs/CONTROLS_CPP.md):
//   .key     0x178 bytes = char name[16] + 45 * {u32 a; u32 b}
//            the loader uses 31 pairs; `b` is zero in the supply;
//   cfg      0x2000 bytes; magic is compared including the trailing NUL;
//            key names 0x1E0 (9 u32 each, interleaved), axis presets 0x24C,
//            sensitivity 0x258 (milliseconds), buttons 0x264, records 0x27C.
//
// Actions come from the 31-entry pointer table at 0x4BC558 (RVA 0xBC558) and
// key codes from the 123-entry, 6-byte name table at 0x4A5B88.  Both were read
// out of `gundx.exe` 1.36, not guessed.  See the doc for the evidence.

#include "input/controls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <limits>
#include <sstream>
#include <utility>

namespace gm {
namespace {

constexpr char kConfigMagic[] = "GUNMETAL Configuration File. av1.93\x1a";
constexpr std::size_t kConfigMagicSize = 0x25; // includes the trailing NUL
constexpr std::uint32_t kCorruptKey = 0x80000000u;
/// Digital threshold after normalisation.  The epsilon keeps a deadzone edge
/// that lands on 0.49999999999999994 a "held" source.
constexpr double kHeldThreshold = 0.5;

constexpr std::array<std::string_view, kActionCount> kActionNames = {
    "forward",          "back",             "turn_left",        "turn_right",
    "look_center",      "fire",             "look_up",          "look_down",
    "strafe",           "strafe_left",      "strafe_right",     "throttle_up",
    "throttle_down",    "next_weapon",      "prev_weapon",      "activate",
    "next_equipment",   "prev_equipment",   "toggle_equipment", "headlights",
    "force_field",      "mine_detector",    "targeting",        "warp_device",
    "radar",            "mine_layer",       "combat_block",     "auto_sentry",
    "decoy",            "linear_left",      "linear_right",
};

// Documented aliases.  The shipped screen and docs use both spellings.
struct Alias {
    std::string_view name;
    int slot;
};
constexpr std::array<Alias, 8> kAliases = {{
    {"backward", 1},
    {"previous_weapon", 14},
    {"previous_equipment", 17},
    {"toggle_equip", 18},
    {"equip_cycle", 18},
    {"strafe_l", 9},
    {"strafe_r", 10},
    {"throttle", 11},
}};

// Name table at 0x4A5B88: 123 entries * 6 bytes.  Codes 0..0x53 follow the
// familiar scancode order; above that the game uses its own dense numbering,
// which is why UP is 0x6D and LEFT 0x6F rather than 0x48/0x4B.
constexpr std::array<std::string_view, 123> kKeyNames = {
    "NONE",  "ESC",   "1",     "2",     "3",     "4",     "5",     "6",
    "7",     "8",     "9",     "0",     "MINUS", "EQUAL", "BS",    "TAB",
    "Q",     "W",     "E",     "R",     "T",     "Y",     "U",     "I",
    "O",     "P",     "LSQB",  "RSQB",  "ENTER", "LCTRL", "A",     "S",
    "D",     "F",     "G",     "H",     "J",     "K",     "L",     "SEMI",
    "QUOTE", "TILDA", "LSFT",  "BSLSH", "Z",     "X",     "C",     "V",
    "B",     "N",     "M",     "COMMA", "DOT",   "SLASH", "RSFT",  "STAR",
    "LALT",  "SPACE", "CAPS",  "F1",    "F2",    "F3",    "F4",    "F5",
    "F6",    "F7",    "F8",    "F9",    "F10",   "NUMLK", "SCRLK", "NUM7",
    "NUM8",  "NUM9",  "NMNUS", "NUM4",  "NUM5",  "NUM6",  "NPLUS", "NUM1",
    "NUM2",  "NUM3",  "NUM0",  "NPERD", "F11",   "F12",   "F13",   "F14",
    "F15",   "KANA",  "CNVRT", "NOCNV", "YEN",   "NPEQU", "CFLEX", "AT",
    "COLON", "ULINE", "KANJI", "STOP",  "AX",    "UNLBL", "NENTR", "RCTRL",
    "NCOMA", "NSLSH", "PRINT", "RALT",  "HOME",  "UP",    "PG UP", "LEFT",
    "RIGHT", "END",   "DOWN",  "PG DN", "INSRT", "DEL",   "LWIN",  "RWIN",
    "MENU",  "PAUSE", "MACRO",
};

// Axis preset table at 0x4BC468: 18 records {name, mask_lo, mask_hi}.  For the
// controller we only need the named directions; negative values invert.
struct PresetActions {
    int negative = -1;
    int positive = -1;
};
constexpr std::array<PresetActions, 18> kPresetActions = {{
    {-1, -1},                     // 0 Not Used
    {0, 1},                       // 1 Forward/Back
    {2, 3},                       // 2 Accel Turn L/R
    {3, 2},                       // 3 Accel Turn R/L
    {2, 3},                       // 4 Linear Turn L/R
    {3, 2},                       // 5 Linear Turn R/L
    {9, 10},                      // 6 Strafe L/R
    {10, 9},                      // 7 Strafe R/L
    {6, 7},                       // 8 Look Up/Down
    {7, 6},                       // 9 Look Down/Up
    {11, 12},                     // 10 Speed +/- Slide
    {12, 11},                     // 11 Speed -/+ Slide
    {11, 12},                     // 12 Speed +/- Nudge
    {12, 11},                     // 13 Speed -/+ Nudge
    {14, 13},                     // 14 Weapons +/-  (minus prev, plus next)
    {13, 14},                     // 15 Weapons -/+
    {17, 16},                     // 16 Equip Cycle +/-
    {16, 17},                     // 17 Equip Cycle -/+
}};

std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "controls: cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "controls: read error in " + path.string();
        return false;
    }
    return true;
}

std::string trim_name(const std::uint8_t* bytes, std::size_t size) {
    std::size_t n = 0;
    while (n < size && bytes[n] != 0) {
        ++n;
    }
    while (n > 0 && (bytes[n - 1] == ' ' || bytes[n - 1] == '\t')) {
        --n;
    }
    return std::string(reinterpret_cast<const char*>(bytes), n);
}

std::string hex_code(std::uint32_t code) {
    std::ostringstream out;
    out << "0x" << std::hex << code;
    return out.str();
}

Vec2 normalized(double x, double y) {
    const double length = std::hypot(x, y);
    if (length > 1.0) {
        x /= length;
        y /= length;
    }
    return Vec2{x, y};
}

} // namespace

std::array<double, 3> ConfigProfile::sensitivity_seconds() const {
    std::array<double, 3> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<double>(sensitivity_ms[i]) / 1000.0;
        if (out[i] < 0.2) {
            out[i] = 0.2; // loader floor at 0x485272
        }
    }
    return out;
}

std::string_view Controls::action_name(int slot) {
    if (slot < 0 || slot >= kActionCount) {
        return {};
    }
    return kActionNames[static_cast<std::size_t>(slot)];
}

int Controls::action_slot(std::string_view name) {
    for (int i = 0; i < kActionCount; ++i) {
        if (kActionNames[static_cast<std::size_t>(i)] == name) {
            return i;
        }
    }
    for (const Alias& alias : kAliases) {
        if (alias.name == name) {
            return alias.slot;
        }
    }
    return -1;
}

bool Controls::valid_key(int code) {
    return code >= 0 && code < static_cast<int>(kKeyNames.size());
}

std::string Controls::key_name(int code) {
    if (code < 0) {
        return "NONE";
    }
    if (valid_key(code)) {
        return std::string(kKeyNames[static_cast<std::size_t>(code)]);
    }
    return "KEY_" + std::to_string(code);
}

Controls::Controls() {
    // Shipped PERFECT (KEYCFG/PERFECT.key): W/S, arrows, Ctrl fire, Space
    // activate.  Slots 29/30 ("Linear Left/Right") are unbound (code 0).
    constexpr std::array<std::uint32_t, kActionCount> kPerfect = {
        0x11, 0x1F, 0x6F, 0x70, 0x2E, 0x1D, 0x6D, 0x72, 0x38, 0x1E, 0x20,
        0x12, 0x10, 0x1B, 0x1A, 0x39, 0x28, 0x27, 0x1C, 0x21, 0x22, 0x31,
        0x14, 0x2F, 0x13, 0x32, 0x30, 0x2D, 0x2C, 0x00, 0x00,
    };
    profile_.name = "PERFECT";
    for (int slot = 0; slot < kActionCount; ++slot) {
        const std::uint32_t code = kPerfect[static_cast<std::size_t>(slot)];
        profile_.pairs[static_cast<std::size_t>(slot)].a = code;
        profile_.bindings[static_cast<std::size_t>(slot)] = Binding{
            std::string(action_name(slot)), static_cast<int>(code), -1, -1, false};
        if (code != 0) {
            bindings_.push_back(profile_.bindings[static_cast<std::size_t>(slot)]);
        }
    }
}

bool Controls::fail(std::string* error, std::string message) {
    last_error_ = std::move(message);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

bool Controls::load_key_file(const std::filesystem::path& path, std::string* error) {
    std::vector<std::uint8_t> data;
    std::string message;
    if (!read_file(path, data, message)) {
        return fail(error, std::move(message));
    }
    if (data.size() < static_cast<std::size_t>(kKeyFileSize)) {
        return fail(error, "controls: truncated .key: " + std::to_string(data.size()) +
                               " of " + std::to_string(kKeyFileSize) + " bytes in " +
                               path.string());
    }

    KeyProfile parsed;
    parsed.name = trim_name(data.data(), 16);
    parsed.bytes_read = data.size();
    parsed.have_tail = data.size() > static_cast<std::size_t>(kKeyFileSize);
    if (parsed.name.empty()) {
        return fail(error, "controls: empty profile name in " + path.string());
    }

    std::vector<KeyLocation> unknown;
    for (int i = 0; i < kKeyPairCount; ++i) {
        const std::size_t offset = 16 + 8 * static_cast<std::size_t>(i);
        parsed.pairs[static_cast<std::size_t>(i)] = KeyPair{
            read_u32(data, offset), read_u32(data, offset + 4)};
    }
    for (int slot = 0; slot < kActionCount; ++slot) {
        const std::uint32_t code = parsed.pairs[static_cast<std::size_t>(slot)].a;
        if (code >= kCorruptKey) {
            return fail(error, "controls: bad key code " + hex_code(code) + " in slot " +
                                   std::to_string(slot) + " of " + path.string());
        }
        if (code != 0 && !valid_key(static_cast<int>(code))) {
            unknown.push_back(KeyLocation{slot, code});
        }
        parsed.bindings[static_cast<std::size_t>(slot)] =
            Binding{std::string(action_name(slot)), code == 0 ? -1 : static_cast<int>(code),
                    -1, -1, false};
    }

    install_key_profile(parsed);
    unknown_keys_ = std::move(unknown);
    for (int slot = 0; slot < kActionCount; ++slot) {
        previous_held_[static_cast<std::size_t>(slot)] = false;
        states_[static_cast<std::size_t>(slot)] = ActionState{};
    }
    return true;
}

bool Controls::load_config(const std::filesystem::path& path, std::string* error) {
    std::vector<std::uint8_t> data;
    std::string message;
    if (!read_file(path, data, message)) {
        return fail(error, std::move(message));
    }
    if (data.size() < static_cast<std::size_t>(kConfigSize)) {
        return fail(error, "controls: truncated GUNMETAL.CFG: " +
                               std::to_string(data.size()) + " of " +
                               std::to_string(kConfigSize) + " bytes");
    }
    if (std::memcmp(data.data(), kConfigMagic, kConfigMagicSize - 1) != 0 ||
        data[kConfigMagicSize - 1] != 0) {
        return fail(error, "controls: bad GUNMETAL.CFG magic (expected av1.93)");
    }

    ConfigProfile parsed;
    parsed.raw.assign(data.begin(), data.begin() + kConfigSize);

    // 0x1E0: two names of nine u32 each, interleaved name1/name2.
    std::array<char, 9> name1{};
    std::array<char, 9> name2{};
    for (std::size_t k = 0; k < 9; ++k) {
        name1[k] = static_cast<char>(read_u32(data, 0x1E0 + 8 * k) & 0xFF);
        name2[k] = static_cast<char>(read_u32(data, 0x1E4 + 8 * k) & 0xFF);
    }
    parsed.key_names[0] = trim_name(reinterpret_cast<const std::uint8_t*>(name1.data()),
                                    name1.size());
    parsed.key_names[1] = trim_name(reinterpret_cast<const std::uint8_t*>(name2.data()),
                                    name2.size());

    parsed.joystick_used_by = read_u32(data, 0x248);
    for (std::size_t i = 0; i < 3; ++i) {
        parsed.axis_preset[i] = read_u32(data, 0x24C + 4 * i);
        parsed.sensitivity_ms[i] = read_u32(data, 0x258 + 4 * i);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        parsed.buttons[i] = read_u32(data, 0x264 + 4 * i);
    }

    for (std::size_t i = 0; i < parsed.records.size(); ++i) {
        const std::size_t base = 0x27C + 0x18C * i;
        ControllerRecord& record = parsed.records[i];
        record.slot = read_u32(data, base);
        for (std::size_t b = 0; b < record.buttons.size(); ++b) {
            record.buttons[b] = read_u32(data, base + 0x004 + 4 * b);
        }
        for (std::size_t a = 0; a < record.axes.size(); ++a) {
            const std::size_t at = base + 0x084 + 12 * a;
            record.axes[a] = AxisMask{read_u32(data, at), read_u32(data, at + 4),
                                      read_u32(data, at + 8)};
        }
        for (std::size_t b = 0; b < record.bits.size(); ++b) {
            const std::size_t at = base + 0x0CC + 12 * b;
            record.bits[b] = BitBinding{read_u32(data, at), read_u32(data, at + 4),
                                        read_u32(data, at + 8)};
        }
    }

    config_ = std::move(parsed);
    apply_config_bindings(config_);
    return true;
}

void Controls::install_key_profile(const KeyProfile& profile) {
    std::vector<Binding> kept;
    kept.reserve(bindings_.size() + kActionCount);
    for (const Binding& binding : bindings_) {
        if (binding.key < 0) {
            kept.push_back(binding); // buttons/axes survive a profile switch
        }
    }
    for (int slot = 0; slot < kActionCount; ++slot) {
        const Binding& source = profile.bindings[static_cast<std::size_t>(slot)];
        if (source.key >= 0) {
            kept.push_back(source);
        }
    }
    bindings_ = std::move(kept);
    profile_ = profile;
}

void Controls::apply_config_bindings(const ConfigProfile& config) {
    for (std::size_t i = 0; i < config.buttons.size(); ++i) {
        const std::uint32_t action = config.buttons[i];
        if (action < static_cast<std::uint32_t>(kActionCount)) {
            Binding binding;
            binding.action = std::string(action_name(static_cast<int>(action)));
            binding.button = static_cast<int>(i);
            bind(binding, ConflictPolicy::allow, nullptr);
        }
    }
    for (std::size_t axis = 0; axis < config.axis_preset.size(); ++axis) {
        const std::uint32_t preset = config.axis_preset[axis];
        if (preset >= kPresetActions.size()) {
            continue;
        }
        const PresetActions& actions = kPresetActions[preset];
        if (actions.negative < 0) {
            continue;
        }
        Binding negative;
        negative.action = std::string(action_name(actions.negative));
        negative.axis = static_cast<int>(axis);
        negative.invert = true;
        bind(negative, ConflictPolicy::allow, nullptr);

        Binding positive;
        positive.action = std::string(action_name(actions.positive));
        positive.axis = static_cast<int>(axis);
        bind(positive, ConflictPolicy::allow, nullptr);
    }
}

bool Controls::bind(const Binding& binding, ConflictPolicy policy, std::string* error) {
    const int slot = action_slot(binding.action);
    if (slot < 0) {
        return fail(error, "controls: unknown action '" + binding.action + "'");
    }
    const int sources = (binding.key >= 0 ? 1 : 0) + (binding.axis >= 0 ? 1 : 0) +
                        (binding.button >= 0 ? 1 : 0);
    if (sources != 1) {
        return fail(error, "controls: binding for '" + binding.action +
                               "' must name exactly one of key/axis/button");
    }
    if (binding.key == 0) {
        return fail(error, "controls: key 0 is NONE and cannot be bound");
    }
    if (binding.axis < -1 || binding.button < -1 || binding.key > 0xFFFF) {
        return fail(error, "controls: invalid source on '" + binding.action + "'");
    }

    for (const Binding& existing : bindings_) {
        const int other = action_slot(existing.action);
        if (other == slot) {
            continue;
        }
        bool same = false;
        if (binding.key >= 0) {
            same = existing.key == binding.key;
        } else if (binding.button >= 0) {
            same = existing.button == binding.button;
        } else {
            same = existing.axis == binding.axis && existing.invert == binding.invert;
        }
        if (same) {
            const std::string what =
                binding.key >= 0
                    ? "key " + std::to_string(binding.key) + " (" + key_name(binding.key) + ")"
                    : binding.button >= 0
                          ? "button " + std::to_string(binding.button)
                          : "axis " + std::to_string(binding.axis) +
                                (binding.invert ? " (-)" : " (+)");
            if (policy == ConflictPolicy::reject) {
                return fail(error, "controls: " + what + " already bound to '" +
                                       existing.action + "'");
            }
        }
    }

    for (Binding& existing : bindings_) {
        if (action_slot(existing.action) != slot) {
            continue;
        }
        const bool same_kind = (binding.key >= 0 && existing.key >= 0) ||
                               (binding.axis >= 0 && existing.axis == binding.axis &&
                                existing.invert == binding.invert) ||
                               (binding.button >= 0 && existing.button == binding.button);
        if (same_kind) {
            existing = binding;
            return true;
        }
    }
    bindings_.push_back(binding);
    return true;
}

std::vector<std::string> Controls::conflicts() const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings_.size(); ++j) {
            const Binding& a = bindings_[i];
            const Binding& b = bindings_[j];
            if (a.action == b.action) {
                continue;
            }
            bool same = false;
            std::string what;
            if (a.key >= 0 && b.key >= 0) {
                same = a.key == b.key;
                what = "key " + std::to_string(a.key) + " (" + key_name(a.key) + ")";
            } else if (a.button >= 0 && b.button >= 0) {
                same = a.button == b.button;
                what = "button " + std::to_string(a.button);
            } else if (a.axis >= 0 && b.axis >= 0) {
                same = a.axis == b.axis && a.invert == b.invert;
                what = "axis " + std::to_string(a.axis) + (a.invert ? " (-)" : " (+)");
            }
            if (same) {
                out.push_back(what + ": " + a.action + "/" + b.action);
            }
        }
    }
    return out;
}

bool Controls::set_deadzone(double deadzone, std::string* error) {
    if (!std::isfinite(deadzone) || deadzone < 0.0 || deadzone > 1.0) {
        return fail(error, "controls: deadzone must be finite and in [0, 1]");
    }
    deadzone_ = deadzone;
    return true;
}

void Controls::set_key(int code, bool down) {
    if (code < 0 || code > 0xFFFF) {
        return;
    }
    if (static_cast<std::size_t>(code) >= key_down_.size()) {
        key_down_.resize(static_cast<std::size_t>(code) + 1, 0);
    }
    key_down_[static_cast<std::size_t>(code)] = down ? 1 : 0;
}

void Controls::set_button(int index, bool down) {
    if (index < 0) {
        return;
    }
    if (static_cast<std::size_t>(index) >= button_down_.size()) {
        button_down_.resize(static_cast<std::size_t>(index) + 1, 0);
    }
    button_down_[static_cast<std::size_t>(index)] = down ? 1 : 0;
}

void Controls::set_axis(int index, double value) {
    if (index < 0) {
        return;
    }
    if (static_cast<std::size_t>(index) >= axis_raw_.size()) {
        axis_raw_.resize(static_cast<std::size_t>(index) + 1, 0.0);
    }
    axis_raw_[static_cast<std::size_t>(index)] = value;
}

double Controls::apply_deadzone(double raw) const {
    const double clamped = std::clamp(raw, -1.0, 1.0);
    const double magnitude = std::fabs(clamped);
    if (magnitude <= deadzone_ || deadzone_ >= 1.0) {
        return 0.0;
    }
    const double scaled = (magnitude - deadzone_) / (1.0 - deadzone_);
    return std::copysign(scaled, clamped);
}

bool Controls::update(double dt, std::string* error) {
    if (!std::isfinite(dt) || dt < 0.0) {
        return fail(error, "controls: dt must be finite and >= 0");
    }
    for (std::size_t i = 0; i < axis_raw_.size(); ++i) {
        if (!std::isfinite(axis_raw_[i])) {
            return fail(error, "controls: axis " + std::to_string(i) + " is not finite");
        }
    }

    std::vector<ActionState> next = states_;
    std::vector<bool> held_now(static_cast<std::size_t>(kActionCount), false);
    for (int slot = 0; slot < kActionCount; ++slot) {
        double value = 0.0;
        for (const Binding& binding : bindings_) {
            if (action_slot(binding.action) != slot) {
                continue;
            }
            if (binding.key >= 0) {
                const std::size_t code = static_cast<std::size_t>(binding.key);
                if (code < key_down_.size() && key_down_[code] != 0) {
                    value += 1.0;
                }
            }
            if (binding.button >= 0) {
                const std::size_t index = static_cast<std::size_t>(binding.button);
                if (index < button_down_.size() && button_down_[index] != 0) {
                    value += 1.0;
                }
            }
            if (binding.axis >= 0) {
                const std::size_t index = static_cast<std::size_t>(binding.axis);
                if (index < axis_raw_.size()) {
                    const double raw = binding.invert ? -axis_raw_[index] : axis_raw_[index];
                    value += apply_deadzone(raw);
                }
            }
        }
        value = std::clamp(value, -1.0, 1.0);
        const bool held = std::fabs(value) >= kHeldThreshold - 1e-9;
        ActionState& state = next[static_cast<std::size_t>(slot)];
        state.value = value;
        state.held = held;
        state.pressed = held && !previous_held_[static_cast<std::size_t>(slot)];
        state.released = !held && previous_held_[static_cast<std::size_t>(slot)];
        state.held_time = held ? states_[static_cast<std::size_t>(slot)].held_time + dt : 0.0;
        held_now[static_cast<std::size_t>(slot)] = held;
    }

    states_ = std::move(next);
    previous_held_ = std::move(held_now);
    return true;
}

std::optional<std::string> Controls::map_key(int code) const {
    for (const Binding& binding : bindings_) {
        if (binding.key == code) {
            return binding.action;
        }
    }
    return std::nullopt;
}

std::optional<std::string> Controls::map_button(int index) const {
    for (const Binding& binding : bindings_) {
        if (binding.button == index) {
            return binding.action;
        }
    }
    return std::nullopt;
}

std::optional<Controls::AxisSample> Controls::map_axis(int index, double raw) const {
    if (!std::isfinite(raw)) {
        return std::nullopt;
    }
    for (const Binding& binding : bindings_) {
        if (binding.axis == index) {
            const double value = binding.invert ? -raw : raw;
            return AxisSample{binding.action, apply_deadzone(value)};
        }
    }
    return std::nullopt;
}

ActionState* Controls::find_action(std::string_view name) {
    const int slot = action_slot(name);
    if (slot < 0) {
        return nullptr;
    }
    return &states_[static_cast<std::size_t>(slot)];
}

const ActionState* Controls::find_action(std::string_view name) const {
    const int slot = action_slot(name);
    if (slot < 0) {
        return nullptr;
    }
    return &states_[static_cast<std::size_t>(slot)];
}

const ActionState& Controls::action(std::string_view name) const {
    static const ActionState empty{};
    const ActionState* state = find_action(name);
    return state != nullptr ? *state : empty;
}

bool Controls::has_action(std::string_view name) const { return find_action(name) != nullptr; }

bool Controls::pressed(std::string_view name) const { return action(name).pressed; }

bool Controls::held(std::string_view name) const { return action(name).held; }

bool Controls::released(std::string_view name) const { return action(name).released; }

double Controls::value(std::string_view name) const { return action(name).value; }

Vec2 Controls::move_vector() const {
    const double forward = value("forward") - value("back");
    const double strafe = value("strafe_right") - value("strafe_left");
    return normalized(forward, strafe);
}

Vec2 Controls::look_vector() const {
    const double x = value("turn_right") - value("turn_left") + value("linear_right") -
                     value("linear_left");
    const double y = value("look_down") - value("look_up");
    return normalized(x, y);
}

} // namespace gm
