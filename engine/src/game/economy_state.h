#pragma once

// Money/shop state (`M6.2.6`, slice 2): cash, a caller-supplied price catalog,
// owned counts, mass and the buy/sell arithmetic of `docs/ECONOMY.md`.
//
// Proven by the original (the section signs are `docs/ECONOMY.md`):
//   * money is one i32 per player, `+0x332c` (`0x81c36c`); the only
//     arithmetic on the field clamps at 0 after `ADD_SOURCEPLAYER_CASH` (§2.1,
//     `0x459dd2`/`0x459ddb`), and the HUD prints it as `CASH: %ld`;
//   * a purchase is `flags & 1` **and** `cash >= price`; a failed check is a
//     no-op — cash and the owned bit do not move (§4.2 step 4, §4.3);
//   * a sale credits the *same* price E9 the purchase debits (§4.2 step 3,
//     `0x47b879`/`0x47b881`), which is why the shipped catalog uses
//     `sell == buy`;
//   * `SOURCEPLAYER_SELL_ALL` refunds shields, ammo, weapons and equipment
//     and clears the masks/indexes (§7);
//   * mass is one sum of four terms (shields, weapons, equipment, ammo,
//     `0x4524d0`, §6) and the chassis limit `+0x3338` does **not** gate a
//     purchase: its only reader is the speed penalty `0x4525e7` (§6);
//   * shield price is 50 cash per 100 points with weight 0 (§3), and the
//     chassis package is `base + 4 * shields * shield_price` (`0x479dd0`,
//     §3).
//
// Model (no RE backing; spelled out in `docs/ECONOMY_STATE.md`):
//   * the catalog is caller input — ids, buy/sell prices and mass per unit.
//     The record `flags` (buyable `0x01`, sellable `0x02`, hidden `0x08`,
//     §4.2/§10) are assumed pre-filtered, so `sell()` checks ownership only;
//   * `sell_all()` is the generic per-line refund at `PriceEntry::sell`.  The
//     original walks shields → ammo → weapons → equipment and refunds
//     equipment from the *weapon* table (§7 item 7); the catalog price is
//     authoritative here, so that bug is not reproduced;
//   * `set_mass_limit()` is an opt-in check, off by default, because the
//     original never blocks a purchase on mass (§6);
//   * the stats counters, the error texts, the positive-count rule and the
//     refusal of anything that would leave `int` are model-side;
//   * units: one catalog unit is one item, one weapon, one ammo *lot*
//     (`+0x08`, §3) or 100 shield points.  No discounts, no reputation, no
//     save/`GameRuntime`/UI wiring — this is a pure state machine.
//
// Every failing call leaves cash, owned counts, mass and the other counters
// untouched and sets `last_error()` to a non-empty text.

#include <string>
#include <vector>

namespace gm {

/// One buyable/sellable line of the catalog.
///
/// `buy`/`sell` are cash per unit and `mass` is mass per unit.  A value of 0
/// is legal (`HEADLIGHTS` costs and weighs 0, `MACHINE GUN` costs 0, all
/// shipped ammo weighs 0, §3/§6).
struct PriceEntry {
    int id = 0;   ///< catalog key, unique and >= 0
    int buy = 0;  ///< cash debited per unit (record E9 for items, `+0x44`
                  ///< for weapons, `+0x0c` per ammo lot, §3)
    int sell = 0; ///< cash credited per unit (the same E9 in the original)
    int mass = 0; ///< mass per unit (`+0x28` equipment, `+0x48` weapon, §6)
};

/// Counters of the module.  "purchase"/"sale" count successful operations
/// (one `buy()`/`sell()` call, one inventory line cleared by `sell_all()`).
struct EconomyStats {
    int purchases = 0;      ///< successful `buy()`
    int sales = 0;          ///< successful `sell()` + lines sold by `sell_all()`
    int rejected_funds = 0; ///< `buy()` refused: `cash < buy * count`
    int rejected_mass = 0;  ///< `buy()` refused by the model mass limit
};

class EconomyState {
public:
    /// `set_mass_limit(kNoMassLimit)` restores the proven behaviour: no limit.
    static constexpr int kNoMassLimit = -1;

    /// Install the price catalog.
    ///
    /// Refused with a text in `*error`/`last_error()` and with the object
    /// unchanged on: an empty list, a negative `id`, a repeated `id`, or a
    /// negative `buy`/`sell`/`mass`.  On success the list is sorted by `id`
    /// and the owned counts are cleared (they belong to the old ids); cash,
    /// stats and the mass limit survive.
    bool set_catalog(std::vector<PriceEntry> entries, std::string* error = nullptr);

    /// Catalog in `id` order.
    const std::vector<PriceEntry>& catalog() const { return catalog_; }
    /// Line by id, or nullptr when the id is not in the catalog.
    const PriceEntry* find(int id) const;

    int cash() const { return cash_; }
    /// Set the money field.  A negative value is refused with a text: every
    /// proven writer leaves the field >= 0 (§2.1), and a save is read as-is
    /// (§9.1) — the shipped saves hold 2000.
    bool set_cash(int value);
    /// `ADD_SOURCEPLAYER_CASH` (op 114, `0x459d99`): `cash += delta`, then a
    /// negative result is clamped to 0 (§2.1).  Returns the new cash.
    int add_cash(int delta);

    /// Buy `count` units of `id` (`0x47b8f8`).
    ///
    /// Refused (false, cash/owned/mass unchanged, text in `last_error()`) on:
    /// an unknown id, `count <= 0`, `cash < buy * count` (`rejected_funds`),
    /// or — only when a model mass limit is set — `mass + mass*count` above
    /// it (`rejected_mass`).  The order is id → count → funds → mass, i.e.
    /// funds are checked where `0x47b8d3` checks them.
    bool buy(int id, int count = 1);

    /// Sell `count` owned units of `id` (`0x47b881`): `cash += sell * count`.
    /// Refused on an unknown id, `count <= 0`, fewer than `count` owned, or a
    /// cash result outside `int`.  There is no `flags & 2` check here (the
    /// catalog is assumed pre-filtered); ownership is the only gate.
    bool sell(int id, int count = 1);

    /// `SOURCEPLAYER_SELL_ALL` (op 140, `0x45bc1a`) as a generic model: every
    /// owned line is refunded at `sell`, the inventory is cleared (so mass
    /// becomes 0) and the total is added to cash.  Returns the total; 0 when
    /// nothing is owned.  A failed refund leaves the state untouched.
    int sell_all();
    /// Same as above; `false` (with a text) on a null pointer or an `int`
    /// overflow.  `*total` is written only on success.
    bool sell_all(int* total);

    /// Current mass: `Σ mass * owned` over the catalog (§6).
    int mass() const;

    /// `kNoMassLimit` (default) or a non-negative limit in the same units.
    int mass_limit() const { return mass_limit_; }
    /// Set the model mass limit; `kNoMassLimit` disables it.  Values below
    /// `kNoMassLimit` are refused with a text.  This check does not exist in
    /// the original — see §6 of `docs/ECONOMY.md`.
    bool set_mass_limit(int limit);

    /// Owned units of `id`, 0 for an unknown id.
    int owned(int id) const;

    const EconomyStats& stats() const { return stats_; }
    /// Text of the last refusal; empty after a successful call.
    const std::string& last_error() const { return last_error_; }

private:
    bool fail(std::string text);
    int index_of(int id) const;

    std::vector<PriceEntry> catalog_; ///< sorted by id
    std::vector<int> owned_;          ///< parallel to catalog_
    int cash_ = 0;
    int mass_limit_ = kNoMassLimit;
    EconomyStats stats_;
    std::string last_error_;
};

} // namespace gm
