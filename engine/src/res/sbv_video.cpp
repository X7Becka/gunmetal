#include "res/sbv_video.h"

namespace gm {

std::optional<VideoFrame> decode_sbv_frame(std::span<const std::uint8_t> block, int width,
                                           int height, std::string& error) {
    (void)block;
    (void)width;
    (void)height;
    error = "SBV frame decoding is not implemented yet";
    return std::nullopt;
}

} // namespace gm
