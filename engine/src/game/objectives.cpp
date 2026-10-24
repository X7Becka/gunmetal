// Level objectives and secrets (`M6.4.5`, slice 2) -- see objectives.h for the
// proof map and the MODEL notes.  Canon: `docs/OBJECTIVES.md` §2/§3/§5.

#include "game/objectives.h"

#include <climits>
#include <string>
#include <utility>

namespace gm {

// --- Objectives ------------------------------------------------------------

bool Objectives::fail(std::string text, std::string* error) const {
    last_error_ = std::move(text);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

bool Objectives::declare(int count, std::string* error) {
    last_error_.clear();
    if (count < 0) {
        // The original compares the parsed u32 (`cmp eax,0x1e`), so a negative
        // count cannot appear; the model refuses it instead of wrapping.
        return fail("objectives: count " + std::to_string(count) +
                        " is negative, 0..30 expected (OBJECTIVES.md 2.1)",
                    error);
    }
    if (count > kMaxObjectives) {
        // `0x45bdf6` cmp eax,0x1e -> error path `0x45c4c1`.
        return fail("objectives: count " + std::to_string(count) + " exceeds 30, the "
                    "DEFINE_LEVEL_OBJECTIVES cap (0x45bdf6, OBJECTIVES.md 2.1)",
                    error);
    }
    // Proven: 103 writes only the count and the text pointers; the mask of the
    // previous declaration stays (see the header note).
    count_ = count;
    return true;
}

bool Objectives::set(int index, bool done, std::string* error) {
    last_error_.clear();
    if (index < 0 || index >= count_) {
        return fail("objectives: index " + std::to_string(index) + " outside [0, " +
                        std::to_string(count_) + ") (OBJECTIVES.md 2.1/2.2)",
                    error);
    }
    const std::uint32_t bit = std::uint32_t{1} << index;
    if (done) {
        mask_ |= bit; // `0x45be49` or / `0x45be4e` mov
    } else {
        mask_ &= ~bit; // MODEL: opcode 104 only ORs, clearing is ours
    }
    return true;
}

bool Objectives::done(int index) const noexcept {
    if (index < 0 || index >= count_) {
        return false;
    }
    return (mask_ & (std::uint32_t{1} << index)) != 0;
}

std::uint32_t Objectives::declared_mask() const noexcept {
    if (count_ <= 0) {
        return 0;
    }
    // count_ <= kMaxObjectives == 30, so the shift is well defined.
    return mask_ & ((std::uint32_t{1} << count_) - 1u);
}

void Objectives::load_mask(std::uint32_t mask) noexcept {
    // `0x456f50` resets the field, the save sub-script then replays opcode 104.
    mask_ = mask;
    last_error_.clear();
}

bool Objectives::all_done() const noexcept {
    // `0x45e9fa` jle: count <= 0 -> false.
    if (count_ <= 0) {
        return false;
    }
    // `0x45ea00`..`0x45ea17`: every 1<<i for i < count; higher bits ignored.
    const std::uint32_t full = (std::uint32_t{1} << count_) - 1u;
    return declared_mask() == full;
}

void Objectives::reset() noexcept {
    // `0x45709d` count, `0x4570a3` mask.
    count_ = 0;
    mask_ = 0;
    last_error_.clear();
}

// --- Secrets ---------------------------------------------------------------

bool Secrets::fail(std::string text, std::string* error) const {
    last_error_ = std::move(text);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

bool Secrets::add(std::string* error) {
    last_error_.clear();
    if (tally_ == INT_MAX) {
        // MODEL: the original's `inc` wraps; the byte-code cannot get here.
        return fail("secrets: tally at INT_MAX, further increment refused "
                    "(MODEL, OBJECTIVES.md 3.1)",
                    error);
    }
    ++tally_; // `0x4536db` inc / `0x4536dc` mov [eax],ecx
    return true;
}

bool Secrets::set_declared(int declared, std::string* error) {
    last_error_.clear();
    if (declared < 0) {
        // `SET_LEVEL_SECRETS` writes a parsed u32 (`0x45b81e`); a negative
        // value is caller error, not a game state.
        return fail("secrets: declared " + std::to_string(declared) +
                        " is negative, 0..INT_MAX expected (OBJECTIVES.md 3.1)",
                    error);
    }
    declared_ = declared;
    return true;
}

} // namespace gm
