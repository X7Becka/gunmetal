// One frame of screen 31, the new-key-set name dialog (`M8.8` slice 23,
// `ui/new_keyset_render`, docs/NEW_KEYSET_RENDER.md).
//
// The map (docs/UI_SCREENS.md §2) lists no handler for 31; the whole frame is
// painted by `0x48ebb0` (argument: the key-set index), called from `0x48d1a5`
// (entering the state) and `0x48f432` (the state-31 branch of the input pass
// `0x48efc0`).  The test pins from pixels:
//
//   * `rows` 1 / step 0 (`0x67f10c`/`0x662d58` of the handler), the 196x131
//     PLATE copy `0x486050`, the title `NAME OF NEW KEY SET` (`0x4bdd8c`) in
//     font 5 centred at y 14, the name in font 3 at `x = 98 - width/2`, y 28,
//     the four font-0 prompts at x 26 (y 48/58/87/102) and the caret frame
//     (8x17, top row 27, hollow, painted before the name);
//   * the 15-character limit of the field (16 bytes, `0x405b20` with max 0x10
//     and the `strlen >= 15` guard), the `NEW SET` fallback (`0x4bdd3c`) and
//     the caret width rule 8/16/6 (`M`/`W`/`I`);
//   * every pixel of the whole 320x200 canvas is compared with an independent
//     composition (black or the locally decoded PLATE, then the caret frame,
//     then the four text runs in emission order); the numbers are printed;
//   * changing the name changes only the name rows, a 30-character name is cut
//     to 15 and is byte-identical to the 15-character prefix;
//   * `caret = false` removes exactly the 46 pixels of the hollow 8x17 frame;
//   * `background = "PLATE"` is compared with the same decoded panel and leaves
//     the text ink unchanged; everything outside 196x131 stays black;
//   * the exact fade (`fade_lut`): level 11 is byte-identical to the unfaded
//     frame (0/64000), levels 12..15 are four different frames;
//   * negatives: empty game path, no BMP.IDX, no PLATE, no FONTS4 (faces 5/0),
//     no FONTS2 (face 3), a PLATE of the wrong size, an unknown background,
//     `fade_lut` without a table or a dump and a missing dump file — all
//     `new_keyset_render:` texts with a cleared buffer; two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/NEW_KEYSET_RENDER.md §5 pins them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/new_keyset_render.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want
                  << "\")\n";
    }
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// One frame (or one refusal) of the renderer.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::NewKeysetRenderStats stats;
};

Frame run(const fs::path& game, const gm::NewKeysetRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_new_keyset(game, options, frame.rgba, frame.width, frame.height,
                                     &frame.stats, &frame.error);
    return frame;
}

long long differing_pixels(const std::vector<std::uint32_t>& a,
                           const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) {
        return -1;
    }
    long long count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++count;
        }
    }
    return count;
}

bool is_black(std::uint32_t pixel) { return pixel == gm::kNewKeysetBackground; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Differing pixels outside the rows `y0..y1` (inclusive).
long long differing_outside(const Frame& a, const Frame& b, int y0, int y1) {
    long long count = 0;
    for (int y = 0; y < a.height; ++y) {
        if (y >= y0 && y <= y1) {
            continue;
        }
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) != at(b, x, y)) {
                ++count;
            }
        }
    }
    return count;
}

struct Box {
    int x0 = -1;
    int y0 = -1;
    int x1 = -1;
    int y1 = -1;
    int count = 0;

    bool empty() const { return count == 0; }
    int width() const { return empty() ? 0 : x1 - x0 + 1; }
    int height() const { return empty() ? 0 : y1 - y0 + 1; }
};

std::string box_text(const Box& box) {
    if (box.empty()) {
        return "empty";
    }
    return "(" + std::to_string(box.x0) + "," + std::to_string(box.y0) + ")-(" +
           std::to_string(box.x1) + "," + std::to_string(box.y1) + ") " +
           std::to_string(box.width()) + "x" + std::to_string(box.height()) + ", " +
           std::to_string(box.count) + " px";
}

/// Bounding box of the non-black pixels of the rows `y0..y1`.
Box ink_box(const Frame& frame, int y0, int y1, int x_limit = 320, int x_min = 0) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = std::max(0, x_min); x < std::min(frame.width, x_limit); ++x) {
            if (is_black(at(frame, x, y))) {
                continue;
            }
            ++box.count;
            if (box.x0 < 0 || x < box.x0) {
                box.x0 = x;
            }
            if (box.y0 < 0 || y < box.y0) {
                box.y0 = y;
            }
            if (x > box.x1) {
                box.x1 = x;
            }
            if (y > box.y1) {
                box.y1 = y;
            }
        }
    }
    return box;
}

// --- independent composition --------------------------------------------------

/// One 8-bpp `FRONTEND/BMP.IDX` entry as `parse_bmp` returns it (top-down rows,
/// RGBA palette forced opaque).
struct Asset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> indices;
    std::vector<std::uint32_t> palette;

    std::uint32_t colour(int x, int y) const {
        const std::uint8_t index =
            indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)];
        return palette[index];
    }
};

Asset load_asset(const fs::path& game, const std::string& name) {
    Asset asset;
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return asset;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return asset;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return asset;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    if (!image || image->bpp != 8 || image->palette.size() < 4) {
        return asset;
    }
    asset.width = image->width;
    asset.height = image->height;
    asset.indices = image->pixels;
    asset.palette.resize(image->palette.size() / 4u);
    for (std::size_t i = 0; i < asset.palette.size(); ++i) {
        const std::size_t base = i * 4u;
        asset.palette[i] = (static_cast<std::uint32_t>(image->palette[base + 0]) << 24) |
                           (static_cast<std::uint32_t>(image->palette[base + 1]) << 16) |
                           (static_cast<std::uint32_t>(image->palette[base + 2]) << 8) | 0xFFu;
    }
    asset.ok = true;
    return asset;
}

std::uint32_t atlas_colour(const gm::FontAtlas& face, std::uint8_t index) {
    const std::size_t base = static_cast<std::size_t>(index) * 4u;
    return (static_cast<std::uint32_t>(face.palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(face.palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(face.palette[base + 2]) << 8) | 0xFFu;
}

int pen_width(const gm::FontAtlas& face, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += face.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// The expected canvas, composed independently: black, the optional panel, the
/// title (5, centred), the caret frame (hollow, `kNewKeysetCaretIndex`), the
/// name (3) and the four prompts (0) — the emission order of `0x48ebb0`.
/// Glyphs are clipped to the 196x131 band the original writes into, index 0 is
/// transparent.
std::vector<std::uint32_t> expected_canvas(const Asset* plate,
                                           const std::map<int, gm::FontAtlas>& fonts,
                                           std::string_view name, bool caret) {
    const int width = gm::kNewKeysetCanvasWidth;
    const int height = gm::kNewKeysetCanvasHeight;
    std::vector<std::uint32_t> canvas(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      gm::kNewKeysetBackground);
    const auto put = [&](int x, int y, std::uint32_t colour) {
        if (x < 0 || x >= std::min(width, gm::kNewKeysetPanelWidth) || y < 0 ||
            y >= std::min(height, gm::kNewKeysetPanelHeight)) {
            return;
        }
        canvas[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x)] = colour;
    };
    if (plate != nullptr) {
        for (int y = 0; y < gm::kNewKeysetPanelHeight; ++y) {
            for (int x = 0; x < gm::kNewKeysetPanelWidth; ++x) {
                put(x, y, plate->colour(x, y));
            }
        }
    }
    const auto blit = [&](int font, std::string_view text, int x, int y) {
        const gm::FontAtlas& face = fonts.at(font);
        int pen = x;
        for (const char raw : text) {
            const auto ch = static_cast<unsigned char>(raw);
            const gm::FontGlyph* glyph = face.glyph(ch);
            if (glyph != nullptr && glyph->width > 0) {
                for (int row = 0; row < face.cell_height; ++row) {
                    for (int column = 0; column < glyph->width; ++column) {
                        const std::uint8_t index =
                            glyph->pixels[static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(glyph->width) +
                                          static_cast<std::size_t>(column)];
                        if (index == 0) {
                            continue;
                        }
                        put(pen + column, y + row, atlas_colour(face, index));
                    }
                }
            }
            pen += face.advance_of(ch);
        }
    };

    const gm::FontAtlas& title_face = fonts.at(gm::kNewKeysetTitleFont);
    const int title_x = (gm::kNewKeysetPanelWidth -
                         pen_width(title_face, gm::kNewKeysetTitle)) /
                        2;
    blit(gm::kNewKeysetTitleFont, gm::kNewKeysetTitle, title_x, gm::kNewKeysetTitleY);

    const gm::FontAtlas& name_face = fonts.at(gm::kNewKeysetNameFont);
    const int name_width = pen_width(name_face, name);
    const int name_x = gm::kNewKeysetNameCentre - name_width / 2;
    if (caret) {
        const std::uint32_t colour =
            atlas_colour(name_face, gm::kNewKeysetCaretIndex);
        const int left = name_x + name_width;
        const int top = gm::kNewKeysetCaretY;
        const int frame_width = gm::kNewKeysetCaretWidth;
        const int frame_height = gm::kNewKeysetCaretHeight;
        for (int column = 0; column < frame_width; ++column) {
            put(left + column, top, colour);
            put(left + column, top + frame_height - 1, colour);
        }
        for (int row = 1; row < frame_height - 1; ++row) {
            put(left, top + row, colour);
            put(left + frame_width - 1, top + row, colour);
        }
    }
    blit(gm::kNewKeysetNameFont, name, name_x, gm::kNewKeysetNameY);
    blit(gm::kNewKeysetHintFont, gm::kNewKeysetPromptOne, gm::kNewKeysetHintX,
         gm::kNewKeysetPromptOneY);
    blit(gm::kNewKeysetHintFont, gm::kNewKeysetPromptTwo, gm::kNewKeysetHintX,
         gm::kNewKeysetPromptTwoY);
    blit(gm::kNewKeysetHintFont, gm::kNewKeysetEnterHint, gm::kNewKeysetHintX,
         gm::kNewKeysetEnterHintY);
    blit(gm::kNewKeysetHintFont, gm::kNewKeysetEscHint, gm::kNewKeysetHintX,
         gm::kNewKeysetEscHintY);
    return canvas;
}

/// Compare the whole canvas with the independent composition.
void check_canvas(const Frame& frame, const std::vector<std::uint32_t>& expected,
                  const std::string& what) {
    if (frame.rgba.size() != expected.size()) {
        check(false, what + ": canvas size " + std::to_string(frame.rgba.size()));
        return;
    }
    long long wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (frame.rgba[i] != expected[i]) {
            if (wrong == 0) {
                first_x = static_cast<int>(i % static_cast<std::size_t>(frame.width));
                first_y = static_cast<int>(i / static_cast<std::size_t>(frame.width));
            }
            ++wrong;
        }
    }
    check_eq(wrong, 0, what + ": canvas matches the independent composition (first " +
                           std::to_string(first_x) + "," + std::to_string(first_y) + ")");
}

/// Bounding box of an independently composed run, for the printed numbers.
Box expected_box(const std::map<int, gm::FontAtlas>& fonts, int font,
                 std::string_view text, int x, int y) {
    const gm::FontAtlas& face = fonts.at(font);
    Box box;
    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = face.glyph(ch);
        if (glyph != nullptr && glyph->width > 0) {
            for (int row = 0; row < face.cell_height; ++row) {
                for (int column = 0; column < glyph->width; ++column) {
                    const std::uint8_t index =
                        glyph->pixels[static_cast<std::size_t>(row) *
                                          static_cast<std::size_t>(glyph->width) +
                                      static_cast<std::size_t>(column)];
                    if (index == 0) {
                        continue;
                    }
                    const int px = pen + column;
                    const int py = y + row;
                    ++box.count;
                    if (box.x0 < 0 || px < box.x0) {
                        box.x0 = px;
                    }
                    if (box.y0 < 0 || py < box.y0) {
                        box.y0 = py;
                    }
                    if (px > box.x1) {
                        box.x1 = px;
                    }
                    if (py > box.y1) {
                        box.y1 = py;
                    }
                }
            }
        }
        pen += face.advance_of(ch);
    }
    return box;
}

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_new_keyset_render";
    }
    return fs::temp_directory_path() / "gm_new_keyset_render";
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

/// A `BIGFILE` index with `names` packed back to back at offsets 0, size, ...;
/// `dat` gets the bytes in the same order.
bool write_archive(const fs::path& dir, const std::vector<std::string>& names,
                   const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::error_code ec;
    fs::create_directories(dir / "FRONTEND", ec);
    if (ec || names.size() != payloads.size()) {
        return false;
    }
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, static_cast<std::uint32_t>(names.size()));
    std::vector<std::uint8_t> dat;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string name = names[i];
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, static_cast<std::uint32_t>(dat.size()));
        dat.insert(dat.end(), payloads[i].begin(), payloads[i].end());
    }
    return write_bytes(dir / "FRONTEND" / "BMP.IDX", idx) &&
           write_bytes(dir / "FRONTEND" / "BMP.DAT", dat);
}

/// Raw bytes of one entry of the installed `FRONTEND/BMP.IDX`.
std::optional<std::vector<std::uint8_t>> installed_entry(const fs::path& game,
                                                         const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return std::nullopt;
    }
    for (const gm::BigEntry& entry : archive->entries()) {
        if (entry.name == name) {
            return archive->read(entry, error);
        }
    }
    return std::nullopt;
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::NewKeysetRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::NewKeysetRenderStats stats;
    stats.rows = 7;
    stats.chars = 7;
    stats.truncated = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_new_keyset(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "new_keyset_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.chars, 0, what + ": chars cleared");
    check_eq(stats.truncated, 0, what + ": truncated cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // The handler 0x48ebb0 writes both immediates itself.
    check_eq(gm::kNewKeysetRows, 1, tag + "rows = 1 (0x67f10c at 0x48ebb3)");
    check_eq(gm::kNewKeysetRowStep, 0, tag + "step = 0 (0x662d58 at 0x48ebbd)");
    check_eq(gm::kNewKeysetTitleY, 14, tag + "title y 14 (push 0xe at 0x48ebd1)");
    check_eq(gm::kNewKeysetTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kNewKeysetTitleFont, 5, tag + "title font 5 (push 0x5 at 0x48ebdc)");
    check_eq(gm::kNewKeysetNameFont, 3, tag + "name font 3 (push 0x3 at 0x48eca3)");
    check_eq(gm::kNewKeysetNameY, 28, tag + "name y 28 (push 0x1c at 0x48ec9a)");
    check_eq(gm::kNewKeysetNameCentre, 98, tag + "name centre 98 (mov esi,0x62)");
    check_eq(gm::kNewKeysetNameWidth, 196, tag + "name band 196 (push 0xc4)");
    check_eq(gm::kNewKeysetCaretY, 27, tag + "caret row 27 (0x687aa4 = 0x6865f8+27*196)");
    check_eq(gm::kNewKeysetCaretHeight, 17, tag + "caret height 17 (push 0x11)");
    check_eq(gm::kNewKeysetCaretWidth, 8, tag + "caret default width 8 (0x48ebfc)");
    check_eq(gm::kNewKeysetCaretWide, 16, tag + "caret wide 16 (M/W, 0x48ec3b)");
    check_eq(gm::kNewKeysetCaretNarrow, 6, tag + "caret narrow 6 (I, 0x48ec34)");
    check_eq(gm::kNewKeysetMaxChars, 15, tag + "15 characters (0x405c1d with 0x10)");
    check_eq(gm::kNewKeysetHintFont, 0, tag + "prompt font 0");
    check_eq(gm::kNewKeysetHintX, 26, tag + "prompt x 26 (push 0x1a)");
    check_eq(gm::kNewKeysetPromptOneY, 48, tag + "prompt one y 48 (push 0x30)");
    check_eq(gm::kNewKeysetPromptTwoY, 58, tag + "prompt two y 58 (push 0x3a)");
    check_eq(gm::kNewKeysetEnterHintY, 87, tag + "Enter hint y 87 (push 0x57)");
    check_eq(gm::kNewKeysetEscHintY, 102, tag + "Esc hint y 102 (push 0x66)");
    check_eq(gm::kNewKeysetPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kNewKeysetPanelHeight, 131, tag + "PLATE 131 high");
    check_eq(std::string(gm::kNewKeysetTitle), std::string("NAME OF NEW KEY SET"),
             tag + "title 0x4bdd8c");
    check_eq(std::string(gm::kNewKeysetDefault), std::string("NEW SET"),
             tag + "accepted-empty fallback 0x4bdd3c");
    check_eq(std::string(gm::kNewKeysetPromptOne), std::string("TYPE A NAME FOR"),
             tag + "prompt one 0x4bdd7c");
    check_eq(std::string(gm::kNewKeysetPromptTwo), std::string("THE NEW KEY SET"),
             tag + "prompt two 0x4bdd6c");
    check_eq(std::string(gm::kNewKeysetEnterHint),
             std::string("PRESS ENTER TO CONTINUE"), tag + "Enter hint 0x4bd6d4");
    check_eq(std::string(gm::kNewKeysetEscHint), std::string("PRESS ESC TO ABORT"),
             tag + "Esc hint 0x4bd6c0");
    check_eq(std::string(gm::kNewKeysetPanelName), std::string("PLATE"), tag + "PLATE");

    // The name: default, NUL, 15-character limit.
    check_eq(gm::new_keyset_name(gm::NewKeysetRenderOptions{}), std::string("NEW SET"),
             tag + "default name is the fallback");
    gm::NewKeysetRenderOptions empty;
    empty.name.clear();
    check_eq(gm::new_keyset_name(empty), std::string(""), tag + "empty field is empty");
    gm::NewKeysetRenderOptions long_name;
    long_name.name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    check_eq(gm::new_keyset_name(long_name), std::string("ABCDEFGHIJKLMNO"),
             tag + "26 characters -> 15");
    check_eq(static_cast<long long>(gm::new_keyset_name(long_name).size()), 15,
             tag + "the limit is 15 characters");
    gm::NewKeysetRenderOptions c_string;
    c_string.name = std::string("AB\0CD", 5);
    check_eq(gm::new_keyset_name(c_string), std::string("AB"),
             tag + "a NUL ends the name (repnz scas)");

    // x = 98 - width/2, including odd widths (floor, `sar eax,1`).
    check_eq(gm::new_keyset_text_x(0), 98, tag + "text x of an empty run");
    check_eq(gm::new_keyset_text_x(50), 73, tag + "text x of a 50-px run");
    check_eq(gm::new_keyset_text_x(51), 73, tag + "text x of a 51-px run (98-25)");
    check_eq(gm::new_keyset_text_x(240), 98 - 120, tag + "text x may be negative");

    // Caret width rule.
    check_eq(gm::new_keyset_caret_width('\0'), 8, tag + "caret over the NUL is 8");
    check_eq(gm::new_keyset_caret_width('A'), 8, tag + "caret over 'A' is 8");
    check_eq(gm::new_keyset_caret_width('M'), 16, tag + "caret over 'M' is 16");
    check_eq(gm::new_keyset_caret_width('W'), 16, tag + "caret over 'W' is 16");
    check_eq(gm::new_keyset_caret_width('I'), 6, tag + "caret over 'I' is 6");
}

// --- the whole frame ---------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const Frame frame = run(game, gm::NewKeysetRenderOptions{});
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kNewKeysetCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kNewKeysetCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, 1, tag + "stats rows 1");
    check_eq(frame.stats.chars, 7, tag + "stats chars 7 (\"NEW SET\")");
    check_eq(frame.stats.truncated, 0, tag + "stats truncated 0");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "has ink");

    check_canvas(frame, expected_canvas(nullptr, fonts, "NEW SET", true),
                 tag + "default");

    // Everything outside the 196x131 band is untouched black.
    long long outside = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (x < gm::kNewKeysetPanelWidth && y < gm::kNewKeysetPanelHeight) {
                continue;
            }
            outside += is_black(at(frame, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "canvas outside the band is black");

    // Printed geometry: every run, from an independent measure.
    const gm::FontAtlas& title_face = fonts.at(gm::kNewKeysetTitleFont);
    const int title_x =
        (gm::kNewKeysetPanelWidth - pen_width(title_face, gm::kNewKeysetTitle)) / 2;
    const Box title = ink_box(frame, gm::kNewKeysetTitleY,
                              gm::kNewKeysetTitleY + title_face.cell_height - 1,
                              gm::kNewKeysetPanelWidth);
    const Box want_title = expected_box(fonts, gm::kNewKeysetTitleFont,
                                       gm::kNewKeysetTitle, title_x, gm::kNewKeysetTitleY);
    check_eq(title.x0, want_title.x0, tag + "title bbox x0");
    check_eq(title.x1, want_title.x1, tag + "title bbox x1");
    check_eq(title.y0, want_title.y0, tag + "title bbox y0");
    check_eq(title.y1, want_title.y1, tag + "title bbox y1");
    std::printf("  new_keyset title  x=%3d y=%3d \"%s\" ink=%d bbox %s\n", title_x,
                gm::kNewKeysetTitleY, std::string(gm::kNewKeysetTitle).c_str(),
                want_title.count, box_text(title).c_str());

    struct Prompt {
        std::string_view text;
        int y;
    };
    const Prompt prompts[] = {
        {gm::kNewKeysetPromptOne, gm::kNewKeysetPromptOneY},
        {gm::kNewKeysetPromptTwo, gm::kNewKeysetPromptTwoY},
        {gm::kNewKeysetEnterHint, gm::kNewKeysetEnterHintY},
        {gm::kNewKeysetEscHint, gm::kNewKeysetEscHintY},
    };
    const gm::FontAtlas& hint_face = fonts.at(gm::kNewKeysetHintFont);
    for (const Prompt& prompt : prompts) {
        const Box box = ink_box(frame, prompt.y, prompt.y + hint_face.cell_height - 1,
                                gm::kNewKeysetPanelWidth);
        const Box want = expected_box(fonts, gm::kNewKeysetHintFont, prompt.text,
                                      gm::kNewKeysetHintX, prompt.y);
        check_eq(box.x0, want.x0,
                 tag + "prompt \"" + std::string(prompt.text) + "\" bbox x0");
        check_eq(box.x1, want.x1,
                 tag + "prompt \"" + std::string(prompt.text) + "\" bbox x1");
        check_eq(box.y0, want.y0,
                 tag + "prompt \"" + std::string(prompt.text) + "\" bbox y0");
        check_eq(box.y1, want.y1,
                 tag + "prompt \"" + std::string(prompt.text) + "\" bbox y1");
        std::printf("  new_keyset prompt x=%3d y=%3d \"%s\" ink=%d bbox %s\n",
                    gm::kNewKeysetHintX, prompt.y, std::string(prompt.text).c_str(),
                    want.count, box_text(box).c_str());
    }

    // The name line and the caret: the name bbox stops at the caret column, the
    // caret is the hollow 8x17 frame of `0x485d40`.
    const gm::FontAtlas& name_face = fonts.at(gm::kNewKeysetNameFont);
    const int name_width = pen_width(name_face, "NEW SET");
    const int name_x = gm::kNewKeysetNameCentre - name_width / 2;
    const Box name_box = ink_box(frame, gm::kNewKeysetNameY,
                                 gm::kNewKeysetNameY + name_face.cell_height - 1,
                                 name_x + name_width);
    const Box want_name = expected_box(fonts, gm::kNewKeysetNameFont, "NEW SET", name_x,
                                      gm::kNewKeysetNameY);
    check_eq(name_box.x0, want_name.x0, tag + "name bbox x0");
    check_eq(name_box.x1, want_name.x1, tag + "name bbox x1");
    check_eq(name_box.y0, want_name.y0, tag + "name bbox y0");
    check_eq(name_box.y1, want_name.y1, tag + "name bbox y1");
    std::printf("  new_keyset name   x=%3d y=%3d \"NEW SET\" w=%d ink=%d bbox %s\n", name_x,
                gm::kNewKeysetNameY, name_width, want_name.count,
                box_text(name_box).c_str());

    // Caret = the difference to the caret-less frame: 46 px of a hollow 8x17.
    gm::NewKeysetRenderOptions no_caret;
    no_caret.caret = false;
    const Frame bare = run(game, no_caret);
    check(bare.ok, tag + "caret-off frame: " + bare.error);
    if (!bare.ok) {
        return;
    }
    check_eq(differing_pixels(frame.rgba, bare.rgba), 46,
             tag + "the caret adds the 46 pixels of a hollow 8x17 frame");
    check_eq(differing_outside(bare, frame, gm::kNewKeysetCaretY,
                               gm::kNewKeysetCaretY + gm::kNewKeysetCaretHeight - 1),
             0, tag + "the caret lives only in rows 27..43");
    check_eq(bare.stats.ink_pixels, frame.stats.ink_pixels,
             tag + "the caret is not glyph ink");
    check_canvas(bare, expected_canvas(nullptr, fonts, "NEW SET", false),
                 tag + "caret off");
    const Box caret_box = ink_box(frame, gm::kNewKeysetCaretY,
                                  gm::kNewKeysetCaretY + gm::kNewKeysetCaretHeight - 1,
                                  name_x + name_width + gm::kNewKeysetCaretWidth,
                                  name_x + name_width);
    check_eq(caret_box.x0, name_x + name_width, tag + "caret x");
    check_eq(caret_box.y0, gm::kNewKeysetCaretY, tag + "caret y");
    check_eq(caret_box.x1, name_x + name_width + gm::kNewKeysetCaretWidth - 1,
             tag + "caret right edge");
    check_eq(caret_box.y1, gm::kNewKeysetCaretY + gm::kNewKeysetCaretHeight - 1,
             tag + "caret bottom edge");
    std::printf("  new_keyset caret  x=%3d y=%3d %dx%d hollow: %d px (diff to "
                "--no-caret: 46)\n",
                name_x + name_width, gm::kNewKeysetCaretY, gm::kNewKeysetCaretWidth,
                gm::kNewKeysetCaretHeight, caret_box.count);

    // The empty field: the fresh dialog (`0x48d19b` clears it), caret at x 98.
    gm::NewKeysetRenderOptions fresh;
    fresh.name.clear();
    const Frame empty = run(game, fresh);
    check(empty.ok, tag + "empty name: " + empty.error);
    if (empty.ok) {
        check_eq(empty.stats.chars, 0, tag + "empty name has 0 chars");
        check_eq(empty.stats.truncated, 0, tag + "empty name is not truncated");
        const Box empty_line = ink_box(empty, gm::kNewKeysetNameY,
                                       gm::kNewKeysetNameY + name_face.cell_height - 1,
                                       gm::kNewKeysetNameCentre);
        check(empty_line.empty(),
              tag + "empty name paints no glyph left of the caret");
        check_canvas(empty, expected_canvas(nullptr, fonts, "", true), tag + "empty");
        std::printf("  new_keyset empty  x=%3d y=%3d caret %dx%d, ink=%d\n",
                    gm::kNewKeysetNameCentre, gm::kNewKeysetCaretY,
                    gm::kNewKeysetCaretWidth, gm::kNewKeysetCaretHeight,
                    empty.stats.ink_pixels);
    }

    std::printf("  new_keyset frame: rows=%d chars=%d ink=%d missing=%d arrows=%d\n",
                frame.stats.rows, frame.stats.chars, frame.stats.ink_pixels,
                frame.stats.missing_glyphs, frame.stats.arrows);
}

// --- the name: change, limit --------------------------------------------------
void test_name(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "name: ";
    const Frame base = run(game, gm::NewKeysetRenderOptions{});
    if (!base.ok) {
        check(false, tag + "default frame: " + base.error);
        return;
    }

    // A five-digit name changes exactly the name/caret rows.
    gm::NewKeysetRenderOptions digits;
    digits.name = "12345";
    const Frame numbered = run(game, digits);
    check(numbered.ok, tag + "digits frame: " + numbered.error);
    if (!numbered.ok) {
        return;
    }
    check_eq(numbered.stats.chars, 5, tag + "digits chars 5");
    const long long diff = differing_pixels(base.rgba, numbered.rgba);
    check(diff > 0, tag + "a different name changes the frame");
    check_eq(differing_outside(base, numbered, gm::kNewKeysetCaretY,
                               gm::kNewKeysetNameY + 16 - 1),
             0, tag + "only rows 27..43 differ");
    check_canvas(numbered, expected_canvas(nullptr, fonts, "12345", true), tag + "digits");
    const long long ink_digits = numbered.stats.ink_pixels;
    std::printf("  new_keyset name \"12345\": %lld px differ from the default, ink=%lld "
                "(default %d)\n",
                diff, ink_digits, base.stats.ink_pixels);

    // The 15-character limit: 30 characters are cut, and the frame is exactly
    // the frame of the 15-character prefix.
    gm::NewKeysetRenderOptions long_name;
    long_name.name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123";
    const Frame cut = run(game, long_name);
    check(cut.ok, tag + "long frame: " + cut.error);
    gm::NewKeysetRenderOptions prefix;
    prefix.name = "ABCDEFGHIJKLMNO";
    const Frame same = run(game, prefix);
    check(same.ok, tag + "15-character frame: " + same.error);
    if (!cut.ok || !same.ok) {
        return;
    }
    check_eq(cut.stats.chars, 15, tag + "30 characters are cut to 15");
    check_eq(cut.stats.truncated, 15, tag + "15 characters dropped");
    check_eq(same.stats.truncated, 0, tag + "the prefix drops nothing");
    check_eq(differing_pixels(cut.rgba, same.rgba), 0,
             tag + "a 30-character name paints the 15-character prefix exactly");
    std::printf("  new_keyset name limit: 30 chars -> chars=%d truncated=%d ink=%d, "
                "identical to the 15-char prefix (0 px)\n",
                cut.stats.chars, cut.stats.truncated, cut.stats.ink_pixels);

    // A name wider than the band: x goes negative and the run is clipped, never
    // wrapped into another row (the original would write into row 26).
    gm::NewKeysetRenderOptions wide;
    wide.name = "MMMMMMMMMMMMMMM"; // 15 * 16 px = 240
    const Frame wide_frame = run(game, wide);
    check(wide_frame.ok, tag + "wide frame: " + wide_frame.error);
    if (wide_frame.ok) {
        check(gm::new_keyset_text_x(240) < 0, tag + "a 240-px name has a negative x");
        check_eq(differing_outside(wide_frame, base, gm::kNewKeysetCaretY,
                                   gm::kNewKeysetNameY + 16 - 1),
                 0, tag + "the wide name stays inside rows 27..43 (no wrap into row 26)");
        check_canvas(wide_frame, expected_canvas(nullptr, fonts, wide.name, true),
                     tag + "wide");
        const Box box = ink_box(wide_frame, gm::kNewKeysetNameY,
                                gm::kNewKeysetNameY + 16 - 1, gm::kNewKeysetPanelWidth);
        std::printf("  new_keyset wide name: x=%d, bbox %s (clipped at the band edge)\n",
                    gm::new_keyset_text_x(240), box_text(box).c_str());
    }
}

// --- PLATE as the background --------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE decoded independently");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kNewKeysetPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kNewKeysetPanelHeight, tag + "PLATE 131 high");

    gm::NewKeysetRenderOptions options;
    options.background = std::string(gm::kNewKeysetPanelName);
    const Frame framed = run(game, options);
    check(framed.ok, tag + "PLATE frame: " + framed.error);
    if (!framed.ok) {
        return;
    }
    check_canvas(framed, expected_canvas(&plate, fonts, "NEW SET", true), tag + "PLATE");

    // The panel is a backdrop: the text ink is the same number as on black.
    const Frame black = run(game, gm::NewKeysetRenderOptions{});
    check(black.ok, tag + "black frame: " + black.error);
    if (black.ok) {
        check_eq(framed.stats.ink_pixels, black.stats.ink_pixels,
                 tag + "the panel does not change the glyph ink");
    }
    long long outside = 0;
    for (int y = 0; y < framed.height; ++y) {
        for (int x = 0; x < framed.width; ++x) {
            if (x < gm::kNewKeysetPanelWidth && y < gm::kNewKeysetPanelHeight) {
                continue;
            }
            outside += is_black(at(framed, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "everything outside 196x131 stays black");

    long long black_pixels = 0;
    for (int y = 0; y < plate.height; ++y) {
        for (int x = 0; x < plate.width; ++x) {
            black_pixels += is_black(plate.colour(x, y)) ? 1 : 0;
        }
    }
    std::printf("  new_keyset PLATE: 196x131 (%d px), %lld of them black; "
                "ink=%d (black frame %d); diff to black frame: %lld px\n",
                plate.width * plate.height, black_pixels, framed.stats.ink_pixels,
                black.stats.ink_pixels,
                black.ok ? differing_pixels(black.rgba, framed.rgba) : -1);

    // A PLATE of the wrong size and an unknown background are refusals.
    gm::NewKeysetRenderOptions missing;
    missing.background = "NOSUCH";
    check_refused(game, missing, "unknown background", "NOSUCH");
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kNewKeysetCanvasWidth) *
                                  gm::kNewKeysetCanvasHeight;

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  fade LUT: SKIP (no 1024-byte GAMEPAL: " << palette_error << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, tag + "build_hud_lut: " + error);
        return;
    }
    std::printf("  fade LUT: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    const auto make = [&](bool fade, int level) {
        gm::NewKeysetRenderOptions options;
        options.background = std::string(gm::kNewKeysetPanelName);
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
    };
    const auto outside_panel = [](const Frame& frame) {
        long long count = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (x < gm::kNewKeysetPanelWidth && y < gm::kNewKeysetPanelHeight) {
                    continue;
                }
                count += is_black(at(frame, x, y)) ? 0 : 1;
            }
        }
        return count;
    };

    const Frame plain = make(false, gm::kHudLutIdentityLevel);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas, tag + "canvas 64000");

    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    check_eq(differing_pixels(identity.rgba, plain.rgba), 0,
             tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 11 ink unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        exact[static_cast<std::size_t>(i)] = make(true, level);
        check(exact[static_cast<std::size_t>(i)].ok,
              tag + "level " + std::to_string(level) + ": " +
                  exact[static_cast<std::size_t>(i)].error);
        if (!exact[static_cast<std::size_t>(i)].ok) {
            return;
        }
        check(differing_pixels(exact[static_cast<std::size_t>(i)].rgba, plain.rgba) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(outside_panel(exact[static_cast<std::size_t>(i)]), 0,
                 tag + "level " + std::to_string(level) + " leaves the canvas black");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels, plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are pairwise different");
    std::printf("  fade: level 11 vs no fade: 0/64000 px differ; adjacent 12..15 diffs "
                "%lld/%lld/%lld; vs 11: %lld/%lld/%lld/%lld\n",
                pair12_13, pair13_14, pair14_15,
                differing_pixels(exact[0].rgba, identity.rgba),
                differing_pixels(exact[1].rgba, identity.rgba),
                differing_pixels(exact[2].rgba, identity.rgba),
                differing_pixels(exact[3].rgba, identity.rgba));

    const Frame again = make(true, 13);
    check(again.ok && differing_pixels(again.rgba, exact[1].rgba) == 0,
          tag + "level 13 is deterministic");
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::NewKeysetRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::NewKeysetRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> fonts2 = installed_entry(game, "FONTS2");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> small = installed_entry(game, "CHECK");
    check(fonts4.has_value() && fonts2.has_value() && plate.has_value() && small.has_value(),
          "negatives: FONTS4/FONTS2/PLATE/CHECK bytes available");
    if (!fonts4 || !fonts2 || !plate || !small) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. FONTS4 + FONTS2: PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4", "FONTS2"},
                        {*fonts4, *fonts2}),
          "negatives: no-PLATE archive written");
    check_refused(root / "game-without-plate", gm::NewKeysetRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE + FONTS2: the FONTS4 faces (5 and 0) are missing.
    check(write_archive(root / "game-without-font4", {"PLATE", "FONTS2"},
                        {*plate, *fonts2}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::NewKeysetRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. FONTS4 + PLATE: the 16x16 face 3 of FONTS2 is missing.
    check(write_archive(root / "game-without-font2", {"FONTS4", "PLATE"},
                        {*fonts4, *plate}),
          "negatives: no-FONTS2 archive written");
    check_refused(root / "game-without-font2", gm::NewKeysetRenderOptions{},
                  "archive without FONTS2", "FONTS2");

    // 4. A PLATE of the wrong size is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "FONTS2", "PLATE"},
                        {*fonts4, *fonts2, *small}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::NewKeysetRenderOptions{},
                  "wrong-size PLATE", "expected 196x131");

    // 5. The minimal complete archive renders (no MENUARRW, FILESLOT or CHECK).
    check(write_archive(root / "game-minimal", {"FONTS4", "FONTS2", "PLATE"},
                        {*fonts4, *fonts2, *plate}),
          "negatives: minimal archive written");
    {
        gm::NewKeysetRenderOptions options;
        options.name = "MIN";
        const Frame minimal = run(root / "game-minimal", options);
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 1, "negatives: minimal archive rows 1");
            check_eq(minimal.stats.chars, 3, "negatives: minimal archive chars 3");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  new_keyset minimal archive (FONTS4+FONTS2+PLATE): rows=%d "
                        "chars=%d ink=%d missing=%d arrows=%d\n",
                        minimal.stats.rows, minimal.stats.chars,
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs,
                        minimal.stats.arrows);
        }
    }
    fs::remove_all(root, ec);

    // 6. The exact fade needs a table or a dump.
    gm::NewKeysetRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::NewKeysetRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::NewKeysetRenderOptions{});
    const Frame second = run(game, gm::NewKeysetRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  new_keyset negatives: rows=%d chars=%d truncated=%d ink=%d "
                    "arrows=%d missing=%d\n",
                    first.stats.rows, first.stats.chars, first.stats.truncated,
                    first.stats.ink_pixels, first.stats.arrows,
                    first.stats.missing_glyphs);
    }
}

} // namespace

int main() {
    const char* env = std::getenv("GUNMETAL_DIR");
    std::vector<fs::path> roots;
    if (env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    fs::path game;
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec)) {
            game = root;
            break;
        }
    }

    test_model();
    if (game.empty()) {
        std::cout << "new_keyset_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kNewKeysetTitleFont, gm::kNewKeysetNameFont,
                             gm::kNewKeysetHintFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "new_keyset_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_name(game, fonts);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "new_keyset_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "new_keyset_render: ok (" << checks << " checks)\n";
    return 0;
}
