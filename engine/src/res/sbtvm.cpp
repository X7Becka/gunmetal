#include "res/sbtvm.h"

namespace gm {

std::optional<SbtScript> decode_sbt_script(std::span<const std::uint8_t> code,
                                           std::string& error) {
    (void)code;
    error = "SBT SCRIPT decoding is not implemented yet";
    return std::nullopt;
}

std::string render_sbt_script(const SbtScript& script) {
    (void)script;
    return {};
}

} // namespace gm
