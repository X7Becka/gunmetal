#include "res/sou2.h"

namespace gm {

std::optional<PcmClip> decode_sou2(std::span<const std::uint8_t> blob, std::string& error) {
    (void)blob;
    error = "SOU2 decoding is not implemented yet";
    return std::nullopt;
}

} // namespace gm
