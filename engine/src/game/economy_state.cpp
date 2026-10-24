#include "game/economy_state.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace gm {
namespace {

constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();

std::string id_text(int id) { return std::to_string(id); }

} // namespace

bool EconomyState::fail(std::string text) {
    last_error_ = std::move(text);
    return false;
}

int EconomyState::index_of(int id) const {
    const PriceEntry* entry = find(id);
    if (entry == nullptr) {
        return -1;
    }
    return static_cast<int>(entry - catalog_.data());
}

const PriceEntry* EconomyState::find(int id) const {
    const auto it = std::lower_bound(
        catalog_.begin(), catalog_.end(), id,
        [](const PriceEntry& entry, int key) { return entry.id < key; });
    if (it == catalog_.end() || it->id != id) {
        return nullptr;
    }
    return &*it;
}

bool EconomyState::set_catalog(std::vector<PriceEntry> entries, std::string* error) {
    const auto reject = [&](std::string text) {
        if (error != nullptr) {
            *error = text;
        }
        return fail(std::move(text));
    };

    if (entries.empty()) {
        return reject("catalog: empty");
    }
    for (const PriceEntry& entry : entries) {
        if (entry.id < 0) {
            return reject("catalog: negative id " + id_text(entry.id));
        }
        if (entry.buy < 0) {
            return reject("catalog: negative buy price for id " + id_text(entry.id));
        }
        if (entry.sell < 0) {
            return reject("catalog: negative sell price for id " + id_text(entry.id));
        }
        if (entry.mass < 0) {
            return reject("catalog: negative mass for id " + id_text(entry.id));
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const PriceEntry& a, const PriceEntry& b) { return a.id < b.id; });
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].id == entries[i - 1].id) {
            return reject("catalog: duplicate id " + id_text(entries[i].id));
        }
    }

    catalog_ = std::move(entries);
    owned_.assign(catalog_.size(), 0);
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool EconomyState::set_cash(int value) {
    if (value < 0) {
        return fail("set_cash: negative cash " + id_text(value));
    }
    cash_ = value;
    last_error_.clear();
    return true;
}

int EconomyState::add_cash(int delta) {
    // op 114 (0x459d99): += then clamp at 0 (0x459dd2/0x459ddb).  The int
    // arithmetic saturates at the type bounds, like the original's i32 add.
    const std::int64_t sum = static_cast<std::int64_t>(cash_) + delta;
    cash_ = sum < 0 ? 0 : (sum > kIntMax ? std::numeric_limits<int>::max()
                                         : static_cast<int>(sum));
    last_error_.clear();
    return cash_;
}

bool EconomyState::buy(int id, int count) {
    const int index = index_of(id);
    if (index < 0) {
        return fail("buy: unknown id " + id_text(id));
    }
    if (count <= 0) {
        return fail("buy: count must be positive, got " + id_text(count));
    }
    const PriceEntry& entry = catalog_[static_cast<std::size_t>(index)];

    const std::int64_t new_owned = static_cast<std::int64_t>(owned_[index]) + count;
    if (new_owned > kIntMax) {
        return fail("buy: owned count overflow for id " + id_text(id));
    }
    const std::int64_t cost = static_cast<std::int64_t>(entry.buy) * count;
    if (cost > cash_) {
        ++stats_.rejected_funds;
        return fail("buy: not enough cash for id " + id_text(id) + " (need " +
                    std::to_string(cost) + ", have " + id_text(cash_) + ")");
    }
    const std::int64_t new_mass =
        static_cast<std::int64_t>(mass()) + static_cast<std::int64_t>(entry.mass) * count;
    if (mass_limit_ != kNoMassLimit && new_mass > mass_limit_) {
        ++stats_.rejected_mass;
        return fail("buy: mass limit " + id_text(mass_limit_) + " for id " + id_text(id) +
                    " (mass " + std::to_string(mass()) + " + " +
                    std::to_string(static_cast<std::int64_t>(entry.mass) * count) + ")");
    }
    if (new_mass > kIntMax) {
        return fail("buy: mass overflow for id " + id_text(id));
    }

    cash_ -= static_cast<int>(cost);
    owned_[index] = static_cast<int>(new_owned);
    ++stats_.purchases;
    last_error_.clear();
    return true;
}

bool EconomyState::sell(int id, int count) {
    const int index = index_of(id);
    if (index < 0) {
        return fail("sell: unknown id " + id_text(id));
    }
    if (count <= 0) {
        return fail("sell: count must be positive, got " + id_text(count));
    }
    if (owned_[index] < count) {
        return fail("sell: id " + id_text(id) + ": owned " +
                    id_text(owned_[index]) + ", requested " + id_text(count));
    }
    const PriceEntry& entry = catalog_[static_cast<std::size_t>(index)];
    const std::int64_t refund = static_cast<std::int64_t>(entry.sell) * count;
    if (static_cast<std::int64_t>(cash_) + refund > kIntMax) {
        return fail("sell: cash overflow for id " + id_text(id));
    }

    cash_ += static_cast<int>(refund);
    owned_[index] -= count;
    ++stats_.sales;
    last_error_.clear();
    return true;
}

int EconomyState::sell_all() {
    int total = 0;
    if (!sell_all(&total)) {
        return 0;
    }
    return total;
}

bool EconomyState::sell_all(int* total) {
    if (total == nullptr) {
        return fail("sell_all: null total");
    }
    std::int64_t refund = 0;
    int lines = 0;
    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        if (owned_[i] <= 0) {
            continue;
        }
        refund += static_cast<std::int64_t>(catalog_[i].sell) * owned_[i];
        if (refund > kIntMax) {
            return fail("sell_all: cash overflow");
        }
        ++lines;
    }
    if (static_cast<std::int64_t>(cash_) + refund > kIntMax) {
        return fail("sell_all: cash overflow");
    }

    cash_ += static_cast<int>(refund);
    std::fill(owned_.begin(), owned_.end(), 0);
    stats_.sales += lines;
    *total = static_cast<int>(refund);
    last_error_.clear();
    return true;
}

int EconomyState::mass() const {
    std::int64_t total = 0;
    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        total += static_cast<std::int64_t>(catalog_[i].mass) * owned_[i];
        if (total > kIntMax) {
            return std::numeric_limits<int>::max();
        }
    }
    return static_cast<int>(total);
}

bool EconomyState::set_mass_limit(int limit) {
    if (limit < kNoMassLimit) {
        return fail("set_mass_limit: limit below -1 (" + id_text(limit) + ")");
    }
    mass_limit_ = limit;
    last_error_.clear();
    return true;
}

int EconomyState::owned(int id) const {
    const int index = index_of(id);
    return index < 0 ? 0 : owned_[static_cast<std::size_t>(index)];
}

} // namespace gm
