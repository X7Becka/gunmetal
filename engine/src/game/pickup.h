#pragma once

// Pickup rules (`M6.2.6` slice 3): one picked-up item applied to one
// `PlayerInventoryModel`, with the reward semantics the original delivers from
// the level sub-scripts.  The engine side of a pickup is only a radius scan and
// an event post; every number below comes from the script opcode it runs.
//
// Proven (`docs/PICKUP.md` = P, `docs/VM_EXEC.md` = V, `docs/DAMAGE.md` = D,
// `docs/ECONOMY.md` = E, `docs/CAMPAIGN_FLOW.md` = CF):
//   * pickup is a radius scan of the 300-slot item table, not a collision:
//     `0x469770` walks the slots each tick (`0x445165`, flags 3), compares the
//     distance with the item's own `+0x4c` radius, and posts the trigger
//     `+0x44` (P §0, §4).  The reward is the entity sub-script, and the item is
//     removed by op 39 `REMOVE_SOURCE_ITEM` (`0x469020`), not by this module.
//   * every item is one of the shipped states `-3/-1/0/>0`; `state -1` is a
//     phantom without an object but still triggers by radius (P §3).
//   * armor is the only health pool: `armor +0x3248`, `armor_max +0x3244`,
//     both in hundredths (D §0.1).  `HEAL_SOURCEPLAYER` (op 112, `0x45af95`)
//     adds arg1 and clamps at `armor_max` (`0x45b02a`); a negative arg is
//     rerouted to `damage_player` (`0x45b0a9`).  A health pickup and an armor
//     pickup therefore move the same gauge — `PickupKind::health` is an alias.
//   * shields are four plates `+0x3250..+0x325c` with one `shield_max
//     +0x324c` (D §0.2, P §8/§9).  `REPAIR_SOURCEPLAYER` (op 113, `0x45b13d`)
//     has two proven branches keyed by flag bit 1: with the bit it hands out
//     one point per plate in the fixed rotation front→right→back→left,
//     skipping full plates (`0x45b177..0x45b1bf`); without it, it adds the
//     whole amount to each of the four plates and clamps each at its own max
//     (`0x45b1c3..0x45b1f5`).  This module implements the rotation branch
//     (the one the task's "4 quadrants one by one" describes).
//   * ammo is one i32 per type at `0x81c2dc + 4*type`; op 109
//     `ADD_SOURCEPLAYER_AMMO` (`0x459ca6`) adds the delta and clamps to
//     `[0, DEFINE_AMMO max]` (V §opcodes).
//   * weapons, equipment and keys are bit masks, and the pickups OR exactly one
//     bit: ops 108/111/106 (`0x45bbd7`/`0x45badb`/`0x45bb50`).  A repeat is
//     the same OR with the bit already set — a proven no-op, not an error.
//   * cash is one i32; op 114 `ADD_SOURCEPLAYER_CASH` adds and clamps the
//     result at 0 for a negative delta (E §2.1).
//   * a secret is the per-player tally `+0x33cc` (`0x81c40c`); op 95
//     `INCREMENT_SOURCEPLAYER_SECRET_TALLY` (`0x45b978` → `0x4536b0`) adds
//     exactly one and has no cap — the tally may exceed `SET_LEVEL_SECRETS`
//     (`0x80d8c0`), as CF §5 notes for `secrt2`/`temple`.
//   * the shipped corpus has 36 levels, 1138 direct item records (§6:
//     op 52×401, 53×17, 54×636, 55×84), 448 of them decorative
//     (`trigger = 0`, radius 0) and 690 with a trigger; the radii sum to the
//     same 1138 (P §6).
//
// Model (no RE backing; see `docs/PICKUP_STATE.md`):
//   * `already_full` / `rejected_duplicate` / `rejected_mass` /
//     `rejected_unknown` are classification names for outcomes the original
//     does not report at all.  `already_full` and `rejected_duplicate` are
//     proven *no-ops*, the other two are refusals this module adds.
//   * mass is a model.  E §6 proves the chassis limit `+0x3338` does not gate
//     a purchase — its only reader is the speed penalty `0x4525e7` — so the
//     limit here is opt-in and disabled by default (`kNoMassLimit`).
//   * `armor_max`, `shield_max`, `ammo_max`, `mass_limit` and
//     `secrets_declared` are caller-supplied fields of the mini model; this
//     module reads no save and no `DEFINE_*` table.
//   * `PickupItem::mass` is per item and accumulates into the inventory; the
//     original item record has no mass field (P §2.1), so the value is caller
//     input.
//   * the message text is a diagnostic.  What the player sees in the original
//     is `ADD_SOURCESCREEN_TEXT` from the sub-script (317 uses, P §6); there is
//     no engine-side pickup message, sound (`PLAY_SAMPLE_AT_SOURCE`, 441
//     uses), item removal, respawn (`PUMARKER`, P §7) or HUD hook here.
//   * no `GameRuntime`, `Things`, save or VM wiring: this is a pure state
//     machine over a caller-owned inventory.

#include <array>
#include <set>
#include <string>
#include <vector>

namespace gm {

/// Kind of a pickup, i.e. which script reward the item stands for.
///
/// `armor`/`health` both move `armor` (op 112, D §0.1); `shield` moves the four
/// plates (op 113); `ammo` a per-type stock (op 109); `weapon`/`equipment`/`key`
/// one ownership bit (ops 108/111/106); `cash` (op 114); `secret` the tally
/// (op 95).  `unknown` is the invalid value.
enum class PickupKind {
    armor,
    shield,
    ammo,
    weapon,
    equipment,
    health,
    cash,
    secret,
    key,
    unknown,
};

/// One pickup item.  `id` is the catalog key: weapon/equipment/key id, ammo
/// type or (ignored) a stable item id for the money-like kinds.  `amount` is
/// points/rounds/cash; for `secret` the original adds exactly one and ignores
/// the count (CF §5).  `mass` is model-side (see the header comment).
struct PickupItem {
    PickupKind kind = PickupKind::unknown;
    int id = 0;
    int amount = 0;
    int mass = 0;
};

/// Minimal per-player inventory this module mutates.  Deliberately *not*
/// `gm::Vitals`/`gm::Arsenal`/`GameRuntime`: the class stays a pure state
/// machine, and the extra fields below are documented model inputs.
struct PlayerInventoryModel {
    int armor = 0;        ///< `+0x3248`, hundredths (D §0.1)
    int armor_max = 0;    ///< `+0x3244`, hundredths; chassis `op3 * 100`
    int shield_max = 0;   ///< `+0x324c`, hundredths; chassis `op2 * 100`
    std::array<int, 4> shields{}; ///< `+0x3250..+0x325c`, front/right/back/left
    int cash = 0;         ///< `+0x332c` (E §2.1)
    int mass = 0;         ///< model: Σ mass of taken items
    int mass_limit = -1;  ///< model: -1 = no limit (E §6)
    std::vector<int> weapons; ///< weapon ids, unique, ascending
    std::set<int> equipment;  ///< equipment ids (op 111 bit set)
    std::set<int> keys;       ///< key ids (op 106 bit set)
    std::vector<int> ammo;    ///< rounds per ammo type, index = type
    int ammo_max = 0;         ///< model: per-type stock limit, 0 = uncapped
    int secrets = 0;          ///< `+0x33cc` found-secret tally (CF §5)
    int secrets_declared = 0; ///< `SET_LEVEL_SECRETS` (`0x80d8c0`), no gate
};

/// Outcome of one `PickupRules::take()`.
enum class PickupResult {
    taken,              ///< the inventory changed
    already_full,       ///< armor/shield/ammo already at its cap — proven no-op
    rejected_mass,      ///< model mass limit would be exceeded
    rejected_duplicate, ///< id already owned — proven no-op (OR of a set bit)
    rejected_unknown,   ///< unknown kind, `id < 0`, non-positive amount, bad mass
};

const char* pickup_kind_name(PickupKind kind);
const char* pickup_result_name(PickupResult result);

/// Counters of the module, one per result plus per-kind successes.
struct PickupStats {
    int taken = 0;
    int already_full = 0;
    int rejected_mass = 0;
    int rejected_duplicate = 0;
    int rejected_unknown = 0;
    std::array<int, 10> taken_by_kind{}; ///< indexed by `PickupKind`
    int mass_taken = 0;                  ///< Σ `item.mass` of taken items

    /// Total number of `take()` calls counted.
    int calls() const {
        return taken + already_full + rejected_mass + rejected_duplicate + rejected_unknown;
    }
};

/// Rules engine.  One instance can be reused for many items; counters and
/// `last_message()` are kept until `reset_stats()`.
class PickupRules {
public:
    /// `mass_limit` value that disables the model mass check (E §6: the
    /// original never blocks on mass).
    static constexpr int kNoMassLimit = -1;

    /// Item-table size of the original (`0x12c` = 300, P §2).  Informational:
    /// this module does not own the table.
    static constexpr int kItemSlots = 300;

    /// Apply one item to `inventory`.
    ///
    /// Validation order: unknown kind → `id < 0` → `amount <= 0` →
    /// `mass < 0` → kind-specific no-op (`already_full`/`rejected_duplicate`)
    /// → mass limit (`rejected_mass`) → mutation.  Every refusal leaves the
    /// inventory unchanged.  `message` (and `last_message()`) always receives a
    /// diagnostic, including on success.
    PickupResult take(const PickupItem& item, PlayerInventoryModel& inventory,
                      std::string* message = nullptr);

    const PickupStats& stats() const { return stats_; }
    void reset_stats() { stats_ = PickupStats{}; }
    const std::string& last_message() const { return last_message_; }

    /// Mass of the items this instance has actually taken (`mass_taken`).
    int pickup_mass_total() const { return stats_.mass_taken; }

    /// Sum of the non-negative `mass` fields of `items` (a pure total, no
    /// inventory involved).  Used by the test and by callers that want the
    /// full load of a pickup batch before applying it.
    static int pickup_mass_total(const std::vector<PickupItem>& items);

private:
    PickupStats stats_;
    std::string last_message_;
};

/// Distribute `amount` points over four plates with one shared `max`, one point
/// per plate in the order 0→1→2→3, skipping full plates — the proven
/// `0x45b177..0x45b1bf` rotation.  Returns the number of points actually
/// stored (capacity-limited).
int distribute_shields(std::array<int, 4>& plates, int max, int amount);

} // namespace gm
