#include "res/sbv_video.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm {
namespace {

// ---------------------------------------------------------------------------
// Engine layout constants (see gm/sbv.py and docs/FORMATS.md §11).
// ---------------------------------------------------------------------------
constexpr int kPlaneGap = 0x20;      // padding between the three planes
constexpr int kScratchWords = 256;   // 0xa1c000..0xa1c400 entropy scratch
constexpr int kMaxScratchSamples = 64; // factor 8 block = 48 dwords
constexpr std::uint32_t kMaxWidth = 0x140;
constexpr std::uint32_t kMaxHeight = 1024;

/// Signed 16-bit value stored in a plane.
std::int16_t as_s16(std::uint16_t value) {
    return static_cast<std::int16_t>(value);
}

/// ``_sar16``: arithmetic shift of the low 16 bits, result masked back to 16 bits.
std::uint16_t sar16(std::uint32_t value, int shift) {
    value &= 0xFFFFu;
    const std::int32_t v = (value & 0x8000u) ? static_cast<std::int32_t>(value) - 0x10000
                                             : static_cast<std::int32_t>(value);
    return static_cast<std::uint16_t>((v >> shift) & 0xFFFF);
}

/// Flat scratch image addressed with byte offsets, mirroring Python's ``_Mem``.
class PlaneMemory {
public:
    explicit PlaneMemory(std::size_t size) : data_(size, 0) {}

    std::uint16_t get(int offset) {
        if (offset < 0 || static_cast<std::size_t>(offset) + 2 > data_.size()) {
            in_range_ = false;
            return 0;
        }
        const auto o = static_cast<std::size_t>(offset);
        return static_cast<std::uint16_t>(data_[o]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[o + 1]) << 8);
    }

    void put(int offset, std::uint32_t value) {
        if (offset < 0 || static_cast<std::size_t>(offset) + 2 > data_.size()) {
            in_range_ = false;
            return;
        }
        const auto o = static_cast<std::size_t>(offset);
        data_[o] = static_cast<std::uint8_t>(value & 0xFF);
        data_[o + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    }

    bool in_range() const { return in_range_; }
    const std::vector<std::uint8_t>& bytes() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
    bool in_range_ = true;
};

// ---------------------------------------------------------------------------
// 0xa1d000 entropy decoder state.
// ---------------------------------------------------------------------------
struct DecoderState {
    std::span<const std::uint8_t> src;
    int pos = 0;
    int bc = 0;
    std::uint32_t acc = 0;
    std::int32_t pred1 = 0;
    std::int32_t pred2 = 0;
    std::int32_t sign = 0;
    std::array<std::uint32_t, kScratchWords> scratch{};

    /// MSB-first bit, loading 32-bit little-endian words and zero-padding the tail.
    unsigned bit() {
        --bc;
        if (bc < 0) {
            std::uint32_t word = 0;
            for (int i = 0; i < 4; ++i) {
                const std::size_t at = static_cast<std::size_t>(pos) + static_cast<std::size_t>(i);
                if (at < src.size()) {
                    word |= static_cast<std::uint32_t>(src[at]) << (8 * i);
                }
            }
            pos += 4;
            acc = word;
            bc = 31;
        }
        const unsigned b = acc >> 31;
        acc <<= 1;
        return b;
    }

    unsigned unary() {
        unsigned count = 0;
        while (bit()) {
            ++count;
        }
        return count;
    }

    void set_at(int index, std::uint32_t value) {
        if (index >= 0 && index < kScratchWords) {
            scratch[static_cast<std::size_t>(index)] = value;
        }
    }
};

/// Decode one ``n``-dword block; results are copied into ``values``.
void decode_segment(DecoderState& st, int n, std::array<std::uint32_t, kMaxScratchSamples>& values) {
    const int base = kScratchWords - n;

    std::uint32_t cls = st.unary();
    if (cls != 0 && st.bit()) {
        cls = 0u - cls;
    }
    cls += static_cast<std::uint32_t>(st.pred1);
    st.pred1 = static_cast<std::int32_t>(cls);

    if (cls == 0) {
        for (int i = base; i < kScratchWords; ++i) {
            st.scratch[static_cast<std::size_t>(i)] = 0;
        }
        for (int i = 0; i < n; ++i) {
            values[static_cast<std::size_t>(i)] = st.scratch[static_cast<std::size_t>(base + i)];
        }
        return;
    }

    std::uint32_t exp = st.unary();
    if (exp != 0 && st.bit()) {
        exp = 0u - exp;
    }
    exp += static_cast<std::uint32_t>(st.pred2);
    st.pred2 = static_cast<std::int32_t>(exp);
    const std::uint32_t step = std::uint32_t{1} << (exp & 31u);
    st.sign = st.bit() ? -1 : 0;

    if (cls >= 0x11u) {
        for (int i = 0; i < n; ++i) {
            values[static_cast<std::size_t>(i)] = st.scratch[static_cast<std::size_t>(base + i)];
        }
        return;
    }

    int cur = -4 * n;
    if (cls == 1u || cls == 3u) {
        while (cur < 0) {
            const int index = kScratchWords + (cur >> 2);
            if ((cur & 0xF) == 0 && st.bit() == 0) {
                st.set_at(index, 0);
                st.set_at(index + 1, 0);
                st.set_at(index + 2, 0);
                st.set_at(index + 3, 0);
                cur += 0x10;
                continue;
            }
            std::uint32_t value = 0;
            if (st.bit()) {
                if (cls == 1u) {
                    value = step;
                    if (st.bit()) {
                        value = 0u - value;
                    }
                } else {
                    const unsigned negative = st.bit();
                    value = step;
                    while (st.bit()) {
                        value += step;
                    }
                    if (negative) {
                        value = 0u - value;
                    }
                }
            }
            st.set_at(index, value);
            cur += 4;
        }
    } else {
        // "Family" handlers (classes 2 and 4..16).  No shipped clip uses them;
        // this is the best-effort transliteration of gm/sbv.py.
        const int nb = (cls == 2u) ? 2 : static_cast<int>(cls - 1u);
        while (cur < 0) {
            const int index = kScratchWords + (cur >> 2);
            std::uint32_t value = 0;
            for (int i = 0; i < nb; ++i) {
                value = (value << 1) | st.bit();
            }
            if (value != 0) {
                if ((value & (std::uint32_t{1} << (nb - 1))) != 0) {
                    value -= std::uint32_t{1} << nb;
                }
                st.set_at(index, value);
                cur += 4;
            } else {
                st.set_at(index, 0);
                st.set_at(index + 1, 0);
                st.set_at(index + 2, 0);
                st.set_at(index + 3, 0);
                cur += 0x10;
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        values[static_cast<std::size_t>(i)] = st.scratch[static_cast<std::size_t>(base + i)];
    }
}

// ---------------------------------------------------------------------------
// 0xa1d940 separable inverse lifting.
// ---------------------------------------------------------------------------
void lift_2d(PlaneMemory& m, int p, int width, int height) {
    int edi = p;
    const int stride = 2 * width;
    int outer = height - 2;
    int esi = stride;
    int ecx = edi + 2 * stride;
    for (;;) {
        for (;;) {
            const std::uint16_t bx = m.get(edi + stride);
            std::uint16_t ax = sar16(static_cast<std::uint32_t>(m.get(ecx + stride)) + bx + 2, 2);
            const std::uint16_t bp = static_cast<std::uint16_t>(m.get(ecx) - ax);
            m.put(ecx, bp);
            ax = sar16(static_cast<std::uint32_t>(m.get(edi)) + bp + 1, 1);
            m.put(edi + stride, static_cast<std::uint32_t>(ax) + bx);
            edi += 2;
            ecx += 2;
            esi -= 2;
            if (esi == 0) {
                break;
            }
        }
        ecx = edi;
        esi = stride;
        edi -= stride;
        for (;;) {
            edi += 4;
            ecx += 4;
            esi -= 4;
            if (esi == 0) {
                break;
            }
            for (const int base : {edi, ecx}) {
                std::uint16_t ax =
                    sar16(static_cast<std::uint32_t>(m.get(base - 2)) + m.get(base + 2) + 2, 2);
                const std::uint16_t bp = static_cast<std::uint16_t>(m.get(base) - ax);
                ax = sar16(static_cast<std::uint32_t>(m.get(base - 4)) + bp + 1, 1);
                m.put(base, bp);
                m.put(base - 2, static_cast<std::uint32_t>(m.get(base - 2)) + ax);
            }
        }
        const std::uint16_t ax = m.get(edi - 4);
        const std::uint16_t bx = m.get(ecx - 4);
        m.put(edi - 2, static_cast<std::uint32_t>(m.get(edi - 2)) + ax);
        m.put(ecx - 2, static_cast<std::uint32_t>(m.get(ecx - 2)) + bx);
        edi = ecx;
        esi = stride;
        outer -= 2;
        ecx = edi + 2 * stride;
        if (outer == 0) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 0x49f1e3 lift + bottom-row / right-edge fix-ups.
// ---------------------------------------------------------------------------
void filt_edges(PlaneMemory& m, int p, int width, int height) {
    lift_2d(m, p, width, height);
    const int wh = width * height;
    const int c = wh - width;
    const int d = wh - 2 * width;
    for (int i = 0; i < width; ++i) {
        const int a = p + (c + i) * 2;
        m.put(a, static_cast<std::uint32_t>(m.get(a)) + m.get(p + (d + i) * 2));
    }
    const int row_b = wh - 2 * width;
    const int row_c = wh - width;
    for (int i = 2; i < width; i += 2) {
        for (const int row : {row_b, row_c}) {
            const int t = p + (row + i) * 2;
            std::uint16_t ax =
                sar16(static_cast<std::uint32_t>(m.get(t - 2)) + m.get(t + 2) + 2, 2);
            const std::uint16_t bp = static_cast<std::uint16_t>(m.get(t) - ax);
            ax = sar16(static_cast<std::uint32_t>(m.get(t - 4)) + bp + 1, 1);
            m.put(t, bp);
            m.put(t - 2, static_cast<std::uint32_t>(m.get(t - 2)) + ax);
        }
    }
    const int i = width - 1;
    for (const int row : {row_b, row_c}) {
        const int t = p + (row + i) * 2;
        m.put(t, static_cast<std::uint32_t>(m.get(t)) + m.get(t - 2));
    }
}

// ---------------------------------------------------------------------------
// 0x49f43a gap spread; ``width`` is the *source* row length.
// ---------------------------------------------------------------------------
void spread(PlaneMemory& m, int p, int width, int height) {
    int s = p + (width * height) * 2 - 2;
    int dd = p + (width * height * 4 - 2 * width) * 2 - 4;
    for (int row = height - 1; row >= 0; --row) {
        for (int col = width - 1; col >= 0; --col) {
            m.put(dd, m.get(s));
            s -= 2;
            dd -= 4;
        }
        dd -= width * 4;
    }
}

// ---------------------------------------------------------------------------
// 0x49f4db block scatter.
// ---------------------------------------------------------------------------
void scatter_blocks(PlaneMemory& m, int width, int height, int factor, DecoderState& st,
                    int plane) {
    const int size = (factor * factor * 3) >> 2;
    st.pred1 = 0;
    st.pred2 = 0;
    std::array<std::uint32_t, kMaxScratchSamples> values{};
    for (int i = 0; i < height; i += factor) {
        for (int j = 0; j < width; j += factor) {
            decode_segment(st, size, values);
            int si = 0;
            auto next_sample = [&values, &si]() -> std::uint16_t {
                const std::uint32_t v = values[static_cast<std::size_t>(si)];
                ++si;
                return static_cast<std::uint16_t>(v & 0xFFFFu);
            };

            int q = plane + (i * width + j) * 2;
            if (st.pred1 <= 3 && st.sign != 0) {
                // Path A: blocks laid out column-major inside each step row.
                const int cnt = width * factor - 1;
                for (int k = 0; k < factor; k += 2) {
                    q += width * 2;
                    for (int n2 = 1; n2 < factor; n2 += 2) {
                        m.put(q, next_sample());
                        q += 4 * width;
                    }
                    q -= (cnt + width) * 2;
                    for (int n3 = 0; n3 < factor; ++n3) {
                        m.put(q, next_sample());
                        q += width * 2;
                    }
                    q -= cnt * 2;
                }
            } else {
                // Path B: row-major.
                const int cnt = width - factor;
                for (int k = 0; k < factor; k += 2) {
                    q += 2;
                    for (int n2 = 1; n2 < factor; n2 += 2) {
                        m.put(q, next_sample());
                        q += 4;
                    }
                    q += cnt * 2 - 2;
                    for (int n3 = 0; n3 < factor; ++n3) {
                        m.put(q, next_sample());
                        q += 2;
                    }
                    q += cnt * 2;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Coarse base grid + the per-plane pyramid.
// ---------------------------------------------------------------------------
bool base_grid(PlaneMemory& m, std::span<const std::uint8_t> frame, DecoderState& st, int plane,
               int bw, int bh) {
    if (static_cast<std::size_t>(st.pos) + 8 > frame.size()) {
        return false;
    }
    const auto load_i32 = [&frame](std::size_t at) {
        const std::uint32_t raw = static_cast<std::uint32_t>(frame[at]) |
                                  (static_cast<std::uint32_t>(frame[at + 1]) << 8) |
                                  (static_cast<std::uint32_t>(frame[at + 2]) << 16) |
                                  (static_cast<std::uint32_t>(frame[at + 3]) << 24);
        return static_cast<std::int32_t>(raw);
    };
    const std::int32_t lo = load_i32(static_cast<std::size_t>(st.pos));
    const std::int32_t hi = load_i32(static_cast<std::size_t>(st.pos) + 4);
    st.pos += 8;
    const double scale = (static_cast<double>(hi) - static_cast<double>(lo)) / 256.0;
    int p = st.pos;
    int index = 0;
    while (index < bh * bw) {
        for (int j = 0; j < bw; j += 2) {
            if (static_cast<std::size_t>(p) >= frame.size()) {
                return false;
            }
            const double sample = static_cast<double>(frame[static_cast<std::size_t>(p)]) * scale +
                                  static_cast<double>(lo);
            const auto value = static_cast<std::int64_t>(sample);
            ++p;
            m.put(plane + (index + j) * 2, static_cast<std::uint32_t>(value));
        }
        index += 2 * bw;
    }
    st.pos = p;
    return true;
}

std::size_t plane_buffer_size(int width, int height) {
    const int cw = width / 2;
    const int ch = height / 2;
    return static_cast<std::size_t>(width * height * 2 + kPlaneGap +
                                    2 * (cw * ch * 2 + kPlaneGap));
}

/// 0x49ee8c: reconstruct luma + two chroma planes into one flat buffer.
bool decode_planes(std::span<const std::uint8_t> frame, int width, int height, PlaneMemory& m) {
    DecoderState st;
    st.src = frame;
    int plane = 0;
    const int w4 = width / 4;
    const int h4 = height / 4;
    const int w8 = width / 8;
    const int h8 = height / 8;
    for (int pl = 0; pl < 3; ++pl) {
        st.bc = 0;
        if (pl == 0) {
            if (!base_grid(m, frame, st, plane, w4, h4)) {
                return false;
            }
            scatter_blocks(m, w4, h4, 4, st, plane);
        } else {
            if (!base_grid(m, frame, st, plane, w8, h8)) {
                return false;
            }
            scatter_blocks(m, w8, h8, 4, st, plane);
            filt_edges(m, plane, w8, h8);
            spread(m, plane, w8, h8);
            scatter_blocks(m, w4, h4, 8, st, plane);
        }
        filt_edges(m, plane, w4, h4);
        spread(m, plane, w4, h4);
        scatter_blocks(m, width / 2, height / 2, 8, st, plane);
        filt_edges(m, plane, width / 2, height / 2);
        if (pl == 0) {
            spread(m, plane, width / 2, height / 2);
            scatter_blocks(m, width, height, 8, st, plane);
            filt_edges(m, plane, width, height);
            plane += width * height * 2 + kPlaneGap;
        } else {
            plane += (width / 2) * (height / 2) * 2 + kPlaneGap;
        }
    }
    return m.in_range();
}

/// 0x49e33b: signed-16 Y/C0/C1 planes -> packed 8-bit RGB.
void planes_to_rgb(const PlaneMemory& m, int width, int height, std::vector<std::uint8_t>& rgb) {
    const auto& bytes = m.bytes();
    const auto sample = [&bytes](std::size_t offset) {
        return as_s16(static_cast<std::uint16_t>(bytes[offset]) |
                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
    };
    const int cw = width / 2;
    const int ch = height / 2;
    const std::size_t lum = 0;
    const std::size_t o1 = static_cast<std::size_t>(width * height * 2 + kPlaneGap);
    const std::size_t o2 = o1 + static_cast<std::size_t>(cw * ch * 2 + kPlaneGap);

    rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    std::size_t o = 0;
    for (int y = 0; y < height; ++y) {
        const std::size_t lrow = lum + static_cast<std::size_t>(y * width) * 2;
        const std::size_t crow = static_cast<std::size_t>(y >> 1) * static_cast<std::size_t>(cw) * 2;
        for (int x = 0; x < width; ++x) {
            const int Y = sample(lrow + static_cast<std::size_t>(x) * 2);
            const int c0 = sample(o1 + crow + static_cast<std::size_t>(x >> 1) * 2);
            const int c1 = sample(o2 + crow + static_cast<std::size_t>(x >> 1) * 2);
            int r = (Y + c0 + 8) >> 4;
            int g = (Y - c0 - c1 + 8) >> 4;
            int b = (Y + c1 + 8) >> 4;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            rgb[o] = static_cast<std::uint8_t>(r);
            rgb[o + 1] = static_cast<std::uint8_t>(g);
            rgb[o + 2] = static_cast<std::uint8_t>(b);
            o += 3;
        }
    }
}

} // namespace

std::optional<VideoFrame> decode_sbv_frame(std::span<const std::uint8_t> block, int width,
                                           int height, std::string& error) {
    error.clear();
    if (block.empty()) {
        error = "empty SBV frame block";
        return std::nullopt;
    }
    if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > kMaxWidth ||
        static_cast<std::uint32_t>(height) > kMaxHeight) {
        error = "implausible SBV frame geometry " + std::to_string(width) + "x" +
                std::to_string(height);
        return std::nullopt;
    }
    // The pyramid halves the plane four times; every level must stay even and
    // the smallest stage (width/8 x height/8) must be at least 2x4 so the
    // 2x2 lifting kernel has a stride divisible by four and a non-empty last
    // row.  A width of 8..15 or a height of 16 (all accepted by the old
    // `width >= 8` check) makes lift_2d's decrement-by-four loop spin forever
    // on stride 2.
    if (width % 16 != 0 || height % 16 != 0 || height < 32) {
        error = "unsupported SBV frame geometry " + std::to_string(width) + "x" +
                std::to_string(height);
        return std::nullopt;
    }

    PlaneMemory planes(plane_buffer_size(width, height));
    if (!decode_planes(block, width, height, planes)) {
        error = "truncated SBV frame block";
        return std::nullopt;
    }

    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    planes_to_rgb(planes, width, height, frame.rgb);
    return frame;
}

} // namespace gm
