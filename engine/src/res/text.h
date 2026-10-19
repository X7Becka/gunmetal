#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// ``MUSIC/*.sog``: one sample name per non-empty line.
struct Sog {
    std::vector<std::string> names;
};

std::optional<Sog> parse_sog(std::span<const std::uint8_t> data, std::string& error);

/// ``SOBS/*.soe``: text token stream (grammar is still partial; tokens only).
struct Soe {
    std::vector<std::string> tokens;
};

std::optional<Soe> parse_soe_text(std::span<const std::uint8_t> data, std::string& error);

/// ``.sml``/``.sme`` UI markup: positional Caesar cipher, tags, plain text.
std::vector<std::uint8_t> sml_decode(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> sml_encode(std::span<const std::uint8_t> data);

struct SmlTag {
    std::string name;
    bool closing = false;
    bool self_closing = false;
    std::string raw; // the full "<...>" text
    std::size_t offset = 0;
};

std::vector<SmlTag> sml_tags(std::string_view text);
std::string sml_plain(std::string_view text);

} // namespace gm
