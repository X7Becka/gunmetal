#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gm {

struct BigEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

/// ``BIGFILE`` archive (``.idx`` + ``.dat``), the layout is documented in
/// ``docs/FORMATS.md`` §1 and implemented by ``gm/bigfile.py``.
class BigFile {
public:
    static std::optional<BigFile> open(const std::filesystem::path& idx_path,
                                       std::string& error);

    const std::vector<BigEntry>& entries() const { return entries_; }
    const std::filesystem::path& dat_path() const { return dat_; }
    std::uint64_t dat_size() const { return dat_size_; }

    std::optional<std::vector<std::uint8_t>> read(const BigEntry& entry,
                                                  std::string& error) const;

private:
    std::vector<BigEntry> entries_;
    std::filesystem::path dat_;
    std::uint64_t dat_size_ = 0;
};

} // namespace gm
