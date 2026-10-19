#include "res/bigfile.h"

#include "core/bytes.h"
#include "core/path.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace gm {
namespace {

constexpr std::string_view kMagic = "BIGFILE";

std::optional<std::filesystem::path> find_dat(const std::filesystem::path& idx) {
    const std::filesystem::path base = idx.parent_path() / idx.stem();
    for (const char* ext : {".dat", ".DAT"}) {
        std::error_code ec;
        const auto candidate = base.string() + ext;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::path(candidate);
        }
    }
    std::error_code ec;
    if (const auto resolved = resolve_ci(base.parent_path(), base.filename().string() + ".dat")) {
        return resolved;
    }
    return std::nullopt;
}

} // namespace

std::optional<BigFile> BigFile::open(const std::filesystem::path& idx_path, std::string& error) {
    std::ifstream in(idx_path, std::ios::binary);
    if (!in) {
        error = "cannot open index: " + idx_path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> idx((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    if (idx.size() < 12 || std::memcmp(idx.data(), kMagic.data(), kMagic.size()) != 0) {
        error = "not a BIGFILE index: " + idx_path.string();
        return std::nullopt;
    }
    const std::uint32_t count = rd32(idx.data() + 8);
    const std::size_t need = 12 + static_cast<std::size_t>(count) * 12;
    if (idx.size() < need) {
        error = "index too short for " + std::to_string(count) + " entries";
        return std::nullopt;
    }

    const auto dat = find_dat(idx_path);
    if (!dat) {
        error = "data file not found next to " + idx_path.string();
        return std::nullopt;
    }

    BigFile file;
    file.dat_ = *dat;
    std::error_code ec;
    file.dat_size_ = std::filesystem::file_size(*dat, ec);
    if (ec) {
        error = "cannot stat " + dat->string() + ": " + ec.message();
        return std::nullopt;
    }

    if (file.dat_size_ > 0xFFFFFFFFull) {
        error = "DAT exceeds the 32-bit BIGFILE offset space";
        return std::nullopt;
    }

    file.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* p = idx.data() + 12 + static_cast<std::size_t>(i) * 12;
        BigEntry entry;
        std::size_t name_len = 0;
        while (name_len < 8 && p[name_len] != 0) {
            ++name_len;
        }
        entry.name.assign(reinterpret_cast<const char*>(p), name_len);
        entry.offset = rd32(p + 8);
        const std::uint32_t next =
            (i + 1 < count) ? rd32(idx.data() + 12 + static_cast<std::size_t>(i + 1) * 12 + 8)
                            : static_cast<std::uint32_t>(file.dat_size_);
        if (next < entry.offset || next > file.dat_size_) {
            error = "entry '" + entry.name + "' has a bad offset";
            return std::nullopt;
        }
        entry.size = next - entry.offset;
        file.entries_.push_back(std::move(entry));
    }
    return file;
}

std::optional<std::vector<std::uint8_t>> BigFile::read(const BigEntry& entry,
                                                       std::string& error) const {
    if (entry.offset > dat_size_ || entry.size > dat_size_ - entry.offset) {
        error = "entry '" + entry.name + "' is out of range";
        return std::nullopt;
    }
    std::ifstream in(dat_, std::ios::binary);
    if (!in) {
        error = "cannot open " + dat_.string();
        return std::nullopt;
    }
    in.seekg(entry.offset, std::ios::beg);
    std::vector<std::uint8_t> data(entry.size);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) {
        error = "short read for '" + entry.name + "'";
        return std::nullopt;
    }
    return data;
}

} // namespace gm
