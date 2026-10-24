// One frame of screen 5, `ERROR` (`M8.8` slice 16, `ui/error_render`,
// docs/ERROR_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; the
// screen has no list, no cursor and no items.  The test pins the geometry of
// docs/UI_SCREENS.md §2 and the listing of `0x48acd0`:
//
//   * model: `rows` 0, step 0, the title `ERROR` (font 5, centred in the 196-px
//     band, y 14), the word loop of the error buffer `0x4aa2f8` (font 0,
//     x from 10, y from 34, wrap when `x + width > 180`, pitch 10, 6-px gap)
//     and the prompt `ESC TO RETURN TO MENU` (font 0, centred, y 91);
//   * word splitting: the exact semantics of `0x48ad1a`..`0x48ad85`, including
//     the empty words of leading, doubled and trailing spaces;
//   * the shipped install: the default `.data` message `No Error.` renders,
//     every run's band is compared byte for byte with an independent blit, the
//     bboxes/ink are printed, the whole canvas is compared with an independent
//     composition of the model layout;
//   * the message *is* the dynamic part: the shipped error texts and a
//     wrap-forcing message change the frame (diff counts printed), an empty
//     message drops the words (title + prompt only), a byte without a glyph is
//     counted in `missing_glyphs` and does not fail the frame;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, a `PLATE` of the
//     wrong size, an unknown background entry, `fade_lut` without a table and a
//     dump, and a missing dump file — all `error_render:` texts with a cleared
//     buffer; a `FONTS4`+`PLATE` archive renders; two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/ERROR_RENDER.md §4 pins them.

#include <algorithm>
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
#include "ui/error_render.h"

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
    gm::ErrorRenderStats stats;
};

Frame run(const fs::path& game, const gm::ErrorRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_error(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kErrorBackground; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

bool has_colour(const Frame& frame, int y0, int y1, std::uint32_t colour, int x0 = 0) {
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = std::max(0, x0); x < frame.width; ++x) {
            if (at(frame, x, y) == colour) {
                return true;
            }
        }
    }
    return false;
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
Box ink_box(const Frame& frame, int y0, int y1) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
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

/// Independent blit of one text run into a `width` x `height` band: glyph row
/// `r` at `y + r` (`0x47faf0`), palette index 0 transparent, the atlas palette
/// forced opaque.  0 marks an untouched pixel.
std::vector<std::uint32_t> compose_band(const gm::FontAtlas& atlas, std::string_view text,
                                        int x, int width, int height) {
    std::vector<std::uint32_t> band(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height),
                                    0u);
    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = atlas.glyph(ch);
        if (glyph == nullptr || glyph->width <= 0) {
            pen += atlas.advance_of(ch);
            continue;
        }
        for (int row = 0; row < atlas.cell_height && row < height; ++row) {
            for (int column = 0; column < glyph->width; ++column) {
                const std::uint8_t index =
                    glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column];
                if (index == 0) {
                    continue;
                }
                const int px = pen + column;
                if (px < 0 || px >= width) {
                    continue;
                }
                const std::size_t base = index * 4u;
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(px)] =
                    (static_cast<std::uint32_t>(atlas.palette[base + 0]) << 24) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 1]) << 16) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 2]) << 8) | 0xFFu;
            }
        }
        pen += atlas.advance_of(ch);
    }
    return band;
}

int pen_width(const gm::FontAtlas& face, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += face.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

long long band_ink(const std::vector<std::uint32_t>& band) {
    long long ink = 0;
    for (const std::uint32_t pixel : band) {
        ink += pixel == 0 ? 0 : 1;
    }
    return ink;
}

/// Compare the rows `y..y+cell_height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kErrorPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    long long wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t expected = want == 0 ? gm::kErrorBackground : want;
            if (at(frame, column, y + row) != expected) {
                if (wrong == 0) {
                    first_x = column;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": band matches the independent blit (first x " +
                           std::to_string(first_x) + ")");
}

void check_refused(const fs::path& game, const gm::ErrorRenderOptions& options,
                   const std::string& what, std::string_view needle) {
    std::vector<std::uint32_t> rgba(7, 0xDEADBEEFu);
    int width = 11;
    int height = 13;
    gm::ErrorRenderStats stats;
    stats.rows = 99;
    stats.lines = 77;
    std::string error;
    const bool ok = gm::render_error(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "error_render: "),
          what + ": error is prefixed (got \"" + error + "\")");
    check(contains(error, needle), what + ": error names \"" + std::string(needle) +
                                       "\" (got \"" + error + "\")");
    check_eq(static_cast<long long>(rgba.size()), 0, what + ": buffer cleared");
    check_eq(width, 0, what + ": width 0");
    check_eq(height, 0, what + ": height 0");
    check_eq(stats.rows, 0, what + ": stats zeroed");
    check_eq(stats.lines, 0, what + ": stats lines zeroed");
    check_eq(stats.ink_pixels, 0, what + ": stats ink zeroed");
}

// --- model -------------------------------------------------------------------
/// Independent re-reading of the word loop `0x48ad1a`..`0x48ad85`: split at
/// single `' '` bytes; a space that starts a word (leading, doubled or trailing
/// after a consumed separator) is an empty word, and the space that ends the
/// string is consumed without one.
std::vector<std::string> model_words(std::string_view message) {
    std::vector<std::string> words;
    if (message.empty()) {
        return words;
    }
    std::string text(message);
    std::size_t pos = 0;
    for (;;) {
        const std::size_t space = text.find(' ', pos);
        if (space == std::string::npos) {
            words.push_back(text.substr(pos));
            return words;
        }
        words.push_back(text.substr(pos, space - pos));
        pos = space + 1;
        if (pos == text.size()) {
            return words;
        }
    }
}

struct RunSpec {
    std::string text;
    int font = 0;
    int x = 0;
    int y = 0;
};

/// Independent model of `0x48acd0`: title centred at y 14, the wrapped words of
/// `message` in font 0, prompt centred at y 91.
std::vector<RunSpec> model_layout(std::string_view message,
                                 const gm::FontAtlas& title_face,
                                 const gm::FontAtlas& text_face) {
    std::vector<RunSpec> runs;
    runs.push_back(RunSpec{std::string(gm::kErrorTitle), gm::kErrorTitleFont,
                           std::max(0, (gm::kErrorPanelWidth -
                                        pen_width(title_face, gm::kErrorTitle)) / 2),
                           gm::kErrorTitleY});
    int x = gm::kErrorMessageX;
    int y = gm::kErrorMessageY;
    for (const std::string& word : model_words(message)) {
        const int width = pen_width(text_face, word);
        if (x + width > gm::kErrorMessageWrapWidth) { // 0x48ad48: strictly greater
            x = gm::kErrorMessageX;
            y += gm::kErrorMessageLinePitch;
        }
        runs.push_back(RunSpec{word, gm::kErrorTextFont, x, y});
        x += width + gm::kErrorWordGap;
    }
    runs.push_back(RunSpec{std::string(gm::kErrorPrompt), gm::kErrorTextFont,
                           std::max(0, (gm::kErrorPanelWidth -
                                        pen_width(text_face, gm::kErrorPrompt)) / 2),
                           gm::kErrorPromptY});
    return runs;
}

int model_missing(std::string_view message, const gm::FontAtlas& text_face) {
    int missing = 0;
    for (const char raw : message) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        if (text_face.glyph(ch) == nullptr) {
            ++missing;
        }
    }
    return missing;
}

/// The whole canvas as the model sees it: black, then every run of
/// `model_layout` blitted and clipped to the 196x131 band.
std::vector<std::uint32_t> compose_frame(const std::vector<RunSpec>& runs,
                                         const std::map<int, gm::FontAtlas>& faces) {
    std::vector<std::uint32_t> canvas(
        static_cast<std::size_t>(gm::kErrorCanvasWidth) *
            static_cast<std::size_t>(gm::kErrorCanvasHeight),
        gm::kErrorBackground);
    for (const RunSpec& spec : runs) {
        const auto it = faces.find(spec.font);
        if (it == faces.end()) {
            continue;
        }
        const gm::FontAtlas& atlas = it->second;
        const std::vector<std::uint32_t> band =
            compose_band(atlas, spec.text, spec.x, gm::kErrorPanelWidth, atlas.cell_height);
        for (int row = 0; row < atlas.cell_height; ++row) {
            const int py = spec.y + row;
            if (py < 0 || py >= gm::kErrorPanelHeight) {
                continue;
            }
            for (int column = 0; column < gm::kErrorPanelWidth; ++column) {
                const std::uint32_t pixel =
                    band[static_cast<std::size_t>(row) * static_cast<std::size_t>(gm::kErrorPanelWidth) +
                         static_cast<std::size_t>(column)];
                if (pixel == 0) {
                    continue;
                }
                canvas[static_cast<std::size_t>(py) *
                           static_cast<std::size_t>(gm::kErrorCanvasWidth) +
                       static_cast<std::size_t>(column)] = pixel;
            }
        }
    }
    return canvas;
}

// --- model constants and word splitting --------------------------------------
void test_model() {
    const std::string tag = "model: ";
    check_eq(gm::kErrorCanvasWidth, 320, tag + "canvas width 320");
    check_eq(gm::kErrorCanvasHeight, 200, tag + "canvas height 200");
    check_eq(gm::kErrorPanelWidth, 196, tag + "panel width 196");
    check_eq(gm::kErrorPanelHeight, 131, tag + "panel height 131");
    check_eq(gm::kErrorRows, 0, tag + "rows 0 (0x67f10c, 0x48acd7)");
    check_eq(gm::kErrorItemStep, 0, tag + "step 0 (0x662d58, 0x48acdc)");
    check_eq(gm::kErrorTitleFont, 5, tag + "title font 5");
    check_eq(gm::kErrorTextFont, 0, tag + "message/prompt font 0");

    check_eq(gm::kErrorTitleY, 14, tag + "title y 14 (push 0xe)");
    check_eq(gm::kErrorTitleX, 38, tag + "title x argument 38 (ignored)");
    check_eq(gm::kErrorMessageX, 10, tag + "message x 10 (0x48ad0c)");
    check_eq(gm::kErrorMessageY, 34, tag + "message y 34 (0x48ad04)");
    check_eq(gm::kErrorMessageLinePitch, 10, tag + "line pitch 10 (0x48ad58)");
    check_eq(gm::kErrorMessageWrapWidth, 180, tag + "wrap width 180 (0x48ad48)");
    check_eq(gm::kErrorWordGap, 6, tag + "word gap 6 (0x48ad77)");
    check_eq(gm::kErrorPromptY, 91, tag + "prompt y 91 (push 0x5b)");
    check_eq(gm::kErrorPromptX, 100, tag + "prompt x argument 100 (ignored)");

    check_eq(std::string(gm::kErrorTitle), "ERROR", tag + "title text");
    check_eq(std::string(gm::kErrorPrompt), "ESC TO RETURN TO MENU", tag + "prompt text");
    check_eq(std::string(gm::kErrorDefaultMessage), "No Error.",
             tag + "default message = the .data image of 0x4aa2f8");
    check_eq(std::string(gm::kErrorPanelName), "PLATE", tag + "panel name");

    gm::ErrorRenderOptions defaults;
    check_eq(defaults.message, "No Error.", tag + "options.message default");
    check(defaults.background.empty(), tag + "no background by default");
    check(!defaults.fade_lut, tag + "fade off by default");
    check_eq(defaults.fade_lut_level, gm::kHudLutIdentityLevel, tag + "fade level 11");

    gm::ErrorRenderStats zero;
    check_eq(zero.rows, 0, tag + "stats default rows 0");
    check_eq(zero.lines, 0, tag + "stats default lines 0");
    check_eq(zero.ink_pixels, 0, tag + "stats default ink 0");
    check_eq(zero.missing_glyphs, 0, tag + "stats default missing 0");
    check_eq(zero.arrows, 0, tag + "stats default arrows 0");

    const auto words = [](std::string_view text) {
        std::string out;
        for (const std::string& word : gm::error_words(text)) {
            out += "[" + word + "]";
        }
        return out;
    };
    check_eq(words("No Error."), "[No][Error.]", tag + "words of the default message");
    check_eq(words(""), "", tag + "empty buffer has no words (0x48ad16)");
    check_eq(words("A"), "[A]", tag + "one word");
    check_eq(words(" A"), "[][A]", tag + "leading space is an empty word");
    check_eq(words("A "), "[A]", tag + "one trailing space is consumed without a word");
    check_eq(words("A  B"), "[A][][B]", tag + "doubled space is an empty word");
    check_eq(words("A   B"), "[A][][][B]", tag + "two doubled spaces");
    check_eq(words("A  "), "[A][]", tag + "second of two trailing spaces is an empty word");
    check_eq(words("ESC TO RETURN TO MENU"), "[ESC][TO][RETURN][TO][MENU]",
             tag + "words of the prompt text");
}

// --- shipped install: default frame, bands, composition ----------------------
void test_default_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const Frame frame = run(game, gm::ErrorRenderOptions{});
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kErrorCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kErrorCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()),
             static_cast<long long>(gm::kErrorCanvasWidth) * gm::kErrorCanvasHeight,
             tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kErrorRows, tag + "stats rows 0");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW call)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");

    long long opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, tag + "every pixel opaque");

    const gm::FontAtlas& title = fonts.at(gm::kErrorTitleFont);
    const gm::FontAtlas& text = fonts.at(gm::kErrorTextFont);
    const std::vector<RunSpec> runs = model_layout(gm::kErrorDefaultMessage, title, text);
    check_eq(static_cast<long long>(runs.size()), frame.stats.lines,
             tag + "stats lines = the 0x485dd0 calls of the model");
    check_eq(frame.stats.lines, 4,
             tag + "default message: title + two words + prompt = 4 runs");

    // The whole canvas equals the independent composition of the model.
    const std::vector<std::uint32_t> model = compose_frame(runs, fonts);

    // Every row of the model is compared byte for byte where it carries a
    // single run (title, prompt); rows shared by several words (`0x48ad85`
    // puts them on one line) are covered by the whole-canvas comparison and by
    // the per-row ink below.
    std::string printed;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const RunSpec& spec = runs[i];
        const gm::FontAtlas& face = spec.font == gm::kErrorTitleFont ? title : text;
        const bool alone = std::none_of(runs.begin(), runs.end(), [&](const RunSpec& other) {
            return &other != &spec && other.y == spec.y;
        });
        if (alone) {
            check_band(frame, face, spec.text, spec.x, spec.y,
                       tag + "run " + std::to_string(i) + " \"" + spec.text + "\"");
        }
        printed += (i == 0 ? "" : " | ") + spec.text + " x=" + std::to_string(spec.x) +
                   " y=" + std::to_string(spec.y);
    }

    // One bbox and one ink count per occupied row, checked against the model.
    std::string rows_printed;
    for (int row_index = 0; row_index < static_cast<int>(runs.size()); ++row_index) {
        const RunSpec& spec = runs[static_cast<std::size_t>(row_index)];
        bool seen = false;
        for (int j = 0; j < row_index; ++j) {
            seen = seen || runs[static_cast<std::size_t>(j)].y == spec.y;
        }
        if (seen) {
            continue;
        }
        const gm::FontAtlas& face = spec.font == gm::kErrorTitleFont ? title : text;
        const Box box = ink_box(frame, spec.y, spec.y + face.cell_height - 1);
        long long expected = 0;
        for (int y = spec.y; y < spec.y + face.cell_height; ++y) {
            for (int x = 0; x < gm::kErrorCanvasWidth; ++x) {
                expected += is_black(model[static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(gm::kErrorCanvasWidth) +
                                           static_cast<std::size_t>(x)])
                                ? 0
                                : 1;
            }
        }
        check_eq(box.count, expected, tag + "row y=" + std::to_string(spec.y) +
                                           " ink matches the model");
        rows_printed += (rows_printed.empty() ? "" : " | ") + box_text(box);
    }
    std::printf("    rows: %s\n", rows_printed.c_str());
    check_eq(differing_pixels(frame.rgba, model), 0,
             tag + "whole canvas equals the independent model layout");
    long long ink = 0;
    for (const std::uint32_t pixel : model) {
        ink += is_black(pixel) ? 0 : 1;
    }
    check_eq(frame.stats.ink_pixels, ink, tag + "stats ink = non-black model pixels");
    check_eq(frame.stats.ink_pixels, band_ink(compose_band(title, gm::kErrorTitle,
                                                           runs[0].x, gm::kErrorPanelWidth,
                                                           title.cell_height)) +
                                         band_ink(compose_band(text, "No", runs[1].x,
                                                               gm::kErrorPanelWidth,
                                                               text.cell_height)) +
                                         band_ink(compose_band(text, "Error.", runs[2].x,
                                                               gm::kErrorPanelWidth,
                                                               text.cell_height)) +
                                         band_ink(compose_band(text, gm::kErrorPrompt,
                                                               runs[3].x,
                                                               gm::kErrorPanelWidth,
                                                               text.cell_height)),
             tag + "the four bands are disjoint: ink = their sum");

    const int title_width = pen_width(title, gm::kErrorTitle);
    std::printf("  frame: 320x200, rows=%d lines=%d arrows=%d ink=%d missing=%d; title "
                "%d px wide at x=%d y=%d\n",
                frame.stats.rows, frame.stats.lines, frame.stats.arrows,
                frame.stats.ink_pixels, frame.stats.missing_glyphs, title_width, runs[0].x,
                gm::kErrorTitleY);
    std::printf("    runs: %s\n", printed.c_str());

    // Colour of the faces (docs/MENU_RENDER.md §3): font 5 dark red,
    // font 0 dark grey.
    check(has_colour(frame, gm::kErrorTitleY, gm::kErrorTitleY + title.cell_height - 1,
                     0x9B0F0FFFu),
          tag + "title is font 5 dark red (155,15,15)");
    check(has_colour(frame, gm::kErrorMessageY,
                     gm::kErrorPromptY + text.cell_height - 1, 0x130F0FFFu),
          tag + "message and prompt are font 0 dark grey (19,15,15)");
    check(!has_colour(frame, 0, frame.height - 1, 0xEFEFEFFFu),
          tag + "no font 4 white (every call passes item = -1)");
}

// --- the message is the dynamic part -----------------------------------------
void test_messages(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "message: ";
    const gm::FontAtlas& title = fonts.at(gm::kErrorTitleFont);
    const gm::FontAtlas& text = fonts.at(gm::kErrorTextFont);

    // The shipped error texts (`0x4ab5e4`, `0x4b9fc8`) plus wrap and edge cases.
    const std::vector<std::string> messages = {
        std::string(gm::kErrorDefaultMessage),
        "Unable to load world file (savegame).",
        "Network client program version (1.36) is different from network server "
        "program version (1.35)",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "",
        "A",
        "Unable to engine_initall().",
    };

    const Frame base = run(game, gm::ErrorRenderOptions{});
    check(base.ok, tag + "baseline frame: " + base.error);
    if (!base.ok) {
        return;
    }

    int index = 0;
    for (const std::string& message : messages) {
        gm::ErrorRenderOptions options;
        options.message = message;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "message #" + std::to_string(index) + " renders: " +
                             frame.error);
        ++index;
        if (!frame.ok) {
            continue;
        }
        const std::vector<RunSpec> runs = model_layout(message, title, text);
        check_eq(static_cast<long long>(runs.size()), frame.stats.lines,
                 tag + "lines of \"" + message + "\"");
        check_eq(frame.stats.missing_glyphs, model_missing(message, text),
                 tag + "missing glyphs of \"" + message + "\"");
        check_eq(differing_pixels(frame.rgba, compose_frame(runs, fonts)), 0,
                 tag + "canvas of \"" + message + "\" matches the model layout");
        check_eq(frame.stats.rows, gm::kErrorRows, tag + "rows stay 0");

        int wrap_rows = 0;
        for (const RunSpec& spec : runs) {
            if (spec.font == gm::kErrorTextFont && spec.y != gm::kErrorPromptY) {
                wrap_rows = std::max(wrap_rows, (spec.y - gm::kErrorMessageY) /
                                                    gm::kErrorMessageLinePitch + 1);
            }
        }
        std::printf("  message \"%s\": lines=%d wrap_rows=%d ink=%d missing=%d, diff vs "
                    "default=%lld/64000\n",
                    message.size() > 46 ? (message.substr(0, 43) + "...").c_str()
                                        : message.c_str(),
                    frame.stats.lines, wrap_rows, frame.stats.ink_pixels,
                    frame.stats.missing_glyphs, differing_pixels(frame.rgba, base.rgba));
    }

    // An empty message is the `0x48ad16` branch: title and prompt only.
    gm::ErrorRenderOptions empty;
    empty.message.clear();
    const Frame empty_frame = run(game, empty);
    check(empty_frame.ok, tag + "empty message renders: " + empty_frame.error);
    if (empty_frame.ok) {
        check_eq(empty_frame.stats.lines, 2, tag + "empty message leaves 2 runs");
        const Box all = ink_box(empty_frame, 0, empty_frame.height - 1);
        check(!all.empty() && all.y0 == gm::kErrorTitleY,
              tag + "empty message: top ink is the title at y 14");
        const Box message_rows = ink_box(empty_frame, gm::kErrorMessageY,
                                         gm::kErrorPromptY - 1);
        check(message_rows.empty(),
              tag + "empty message: nothing between y 34 and the prompt (" +
                  box_text(message_rows) + ")");
        check(!all.empty() && all.y1 >= gm::kErrorPromptY &&
                  all.y1 <= gm::kErrorPromptY + text.cell_height - 1,
              tag + "empty message: bottom ink is the prompt row (got " +
                  std::to_string(all.y1) + ")");
        check(differing_pixels(empty_frame.rgba, base.rgba) > 0,
              tag + "empty message changes the frame");
    }

    // The two shipped messages differ from each other and from the default.
    gm::ErrorRenderOptions a;
    a.message = "Unable to load world file (savegame).";
    gm::ErrorRenderOptions b;
    b.message = "Network client program version (1.36) is different from network "
                "server program version (1.35)";
    const Frame fa = run(game, a);
    const Frame fb = run(game, b);
    check(fa.ok && fb.ok, tag + "both shipped messages render");
    if (fa.ok && fb.ok) {
        check(differing_pixels(fa.rgba, fb.rgba) > 0,
              tag + "the two shipped messages differ");
        check(fa.stats.ink_pixels != fb.stats.ink_pixels,
              tag + "their ink differs (" + std::to_string(fa.stats.ink_pixels) + " vs " +
                  std::to_string(fb.stats.ink_pixels) + ")");
        std::printf("  shipped messages: ink %d vs %d, lines %d vs %d, %lld/64000 px "
                    "differ\n",
                    fa.stats.ink_pixels, fb.stats.ink_pixels, fa.stats.lines,
                    fb.stats.lines, differing_pixels(fa.rgba, fb.rgba));
    }

    // A byte without a glyph is skipped and counted, never fatal.
    gm::ErrorRenderOptions missing;
    missing.message = std::string("A") + '\x01' + "B";
    const Frame missing_frame = run(game, missing);
    check(missing_frame.ok, tag + "missing glyph message renders: " + missing_frame.error);
    if (missing_frame.ok) {
        check_eq(missing_frame.stats.missing_glyphs,
                 model_missing(missing.message, text),
                 tag + "the unprintable byte is counted in missing_glyphs");
        check(missing_frame.stats.missing_glyphs > 0,
              tag + "missing_glyphs > 0 for a byte outside the atlas");
        std::printf("  missing glyph: \"A\\x01B\" ink=%d missing=%d lines=%d\n",
                    missing_frame.stats.ink_pixels, missing_frame.stats.missing_glyphs,
                    missing_frame.stats.lines);
    }

    // The wrap threshold `x + width > 180` is strictly greater: with the font 0
    // advance of `A` = 6 px, 28 letters are 168 px (10 + 168 = 178, row 34) and
    // 29 letters are 174 px (10 + 174 = 184, so the word is pushed to row 44).
    for (const auto& [letters, want_y] : std::vector<std::pair<int, int>>{{28, 34}, {29, 44}}) {
        gm::ErrorRenderOptions options;
        options.message = std::string(static_cast<std::size_t>(letters), 'A');
        const Frame frame = run(game, options);
        check(frame.ok, tag + "boundary message renders: " + frame.error);
        if (!frame.ok) {
            continue;
        }
        const std::vector<RunSpec> runs = model_layout(options.message, title, text);
        check_eq(runs[1].y, want_y, tag + std::to_string(letters) + " letters: row " +
                                          std::to_string(want_y));
        check_eq(differing_pixels(frame.rgba, compose_frame(runs, fonts)), 0,
                 tag + std::to_string(letters) + " letters match the model");
        std::printf("  wrap boundary: %d A's = %d px, x+width = %d -> row %d; ink=%d\n",
                    letters, pen_width(text, options.message),
                    gm::kErrorMessageX + pen_width(text, options.message), runs[1].y,
                    frame.stats.ink_pixels);
    }

    // The wrap rule with a long word: `x + width > 180` advances the row even at
    // the start of a line (`0x48ad44`..`0x48ad5a`), the 196-px band clips it.
    gm::ErrorRenderOptions long_word;
    long_word.message = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const Frame long_frame = run(game, long_word);
    check(long_frame.ok, tag + "over-wide word renders: " + long_frame.error);
    if (long_frame.ok) {
        const std::vector<RunSpec> runs = model_layout(long_word.message, title, text);
        check_eq(static_cast<long long>(runs.size()), 3, tag + "one over-wide word -> 3 runs");
        check_eq(runs[1].y, gm::kErrorMessageY + gm::kErrorMessageLinePitch,
                 tag + "an over-wide first word wraps once before it is drawn");
        check_eq(differing_pixels(long_frame.rgba, compose_frame(runs, fonts)), 0,
                 tag + "over-wide word canvas matches the model");
        const Box box = ink_box(long_frame, gm::kErrorMessageY,
                                gm::kErrorMessageY + text.cell_height - 1);
        check(box.empty(), tag + "over-wide word leaves row 34 empty");
        std::printf("  long word: %d px wide, drawn at x=%d y=%d, ink=%d (clipped to 196)\n",
                    pen_width(text, long_word.message), runs[1].x, runs[1].y,
                    long_frame.stats.ink_pixels);
    }
}

// --- PLATE -------------------------------------------------------------------
struct Decoded {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> indices;

    std::uint32_t colour(int x, int y) const {
        const std::uint8_t index =
            indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)];
        const std::size_t base = static_cast<std::size_t>(index) * 4u;
        return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
               (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
               (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
    }
};

std::optional<Decoded> decode_installed(const fs::path& game, const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return std::nullopt;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return std::nullopt;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    if (!image) {
        return std::nullopt;
    }
    Decoded decoded;
    decoded.width = image->width;
    decoded.height = image->height;
    decoded.palette.resize(image->palette.size());
    for (std::size_t i = 0; i + 3 < image->palette.size(); i += 4) {
        decoded.palette[i + 0] = image->palette[i + 0];
        decoded.palette[i + 1] = image->palette[i + 1];
        decoded.palette[i + 2] = image->palette[i + 2];
        decoded.palette[i + 3] = 0xFFu;
    }
    decoded.indices = image->pixels;
    return decoded;
}

void test_plate(const fs::path& game) {
    const std::string tag = "PLATE: ";
    const std::optional<Decoded> plate = decode_installed(game, "PLATE");
    check(plate.has_value() && plate->width == gm::kErrorPanelWidth &&
              plate->height == gm::kErrorPanelHeight,
          tag + "installed PLATE decodes to 196x131");
    if (!plate) {
        return;
    }
    const Frame black_frame = run(game, gm::ErrorRenderOptions{});
    gm::ErrorRenderOptions options;
    options.background = std::string(gm::kErrorPanelName);
    const Frame plate_frame = run(game, options);
    check(black_frame.ok && plate_frame.ok, tag + "both frames render");
    if (!black_frame.ok || !plate_frame.ok) {
        return;
    }
    long long wrong = 0;
    for (int y = 0; y < plate->height; ++y) {
        for (int x = 0; x < plate->width; ++x) {
            if (!is_black(at(black_frame, x, y))) {
                continue; // glyph pixels sit on top of the panel
            }
            const std::uint32_t want = plate->colour(x, y);
            const std::uint32_t got = at(plate_frame, x, y);
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, tag + "PLATE pixels match the independent decode under the ink");
    long long outside = 0;
    for (int y = 0; y < plate_frame.height; ++y) {
        for (int x = 0; x < plate_frame.width; ++x) {
            if (x < plate->width && y < plate->height) {
                continue;
            }
            outside += is_black(at(plate_frame, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "everything outside the 196x131 band stays black");
    check_eq(plate_frame.stats.ink_pixels, black_frame.stats.ink_pixels,
             tag + "text ink unchanged by the backdrop");
    check_eq(plate_frame.stats.lines, black_frame.stats.lines,
             tag + "run count unchanged by the backdrop");
    check(!is_black(at(plate_frame, 0, 0)) || plate->colour(0, 0) == gm::kErrorBackground,
          tag + "frame is not the black canvas");
    std::printf("  PLATE: 196x131 decoded, black vs PLATE diff=%lld px, text ink %d "
                "unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas =
        static_cast<long long>(gm::kErrorCanvasWidth) * gm::kErrorCanvasHeight;

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
        gm::ErrorRenderOptions options;
        options.background = std::string(gm::kErrorPanelName);
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
                if (x < gm::kErrorPanelWidth && y < gm::kErrorPanelHeight) {
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
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 11 ink unchanged");
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
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels,
                 plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are pairwise different");
    std::printf("  fade: level 11 vs no fade: 0/64000 px differ; adjacent 12..15 "
                "diffs %lld/%lld/%lld; vs 11: %lld/%lld/%lld/%lld\n",
                pair12_13, pair13_14, pair14_15,
                differing_pixels(exact[0].rgba, identity.rgba),
                differing_pixels(exact[1].rgba, identity.rgba),
                differing_pixels(exact[2].rgba, identity.rgba),
                differing_pixels(exact[3].rgba, identity.rgba));

    const Frame again = make(true, 13);
    check(again.ok && differing_pixels(again.rgba, exact[1].rgba) == 0,
          tag + "level 13 is deterministic");
}

// --- synthetic archives ------------------------------------------------------
fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_error_render";
    }
    return fs::temp_directory_path() / "gm_error_render";
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

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::ErrorRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::ErrorRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> check_bytes =
        installed_entry(game, "CHECK");
    check(fonts4.has_value() && plate.has_value() && check_bytes.has_value(),
          "negatives: FONTS4/PLATE/CHECK bytes available");
    if (!fonts4 || !plate || !check_bytes) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. An index with FONTS4 only: the faces load, PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::ErrorRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE only: FONTS4 (faces 5/0) is missing.
    check(write_archive(root / "game-without-font4", {"PLATE"}, {*plate}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::ErrorRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE"},
                        {*fonts4, *check_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::ErrorRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE is the whole screen: it must render.
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: minimal archive written");
    {
        const Frame minimal = run(root / "game-minimal", gm::ErrorRenderOptions{});
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 0, "negatives: minimal archive rows 0");
            check_eq(minimal.stats.lines, 4, "negatives: minimal archive lines 4");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  minimal archive (FONTS4+PLATE): ink=%d missing=%d rows=%d "
                        "lines=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs,
                        minimal.stats.rows, minimal.stats.lines);
        }
    }
    fs::remove_all(root, ec);

    // 5. A background name that is not in the archive is a text, not black.
    gm::ErrorRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 6. The exact fade needs a table or a dump.
    gm::ErrorRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::ErrorRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::ErrorRenderOptions{});
    const Frame second = run(game, gm::ErrorRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  negatives: rows=%d lines=%d ink=%d arrows=%d missing=%d\n",
                    first.stats.rows, first.stats.lines, first.stats.ink_pixels,
                    first.stats.arrows, first.stats.missing_glyphs);
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
        std::cout << "error_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kErrorTitleFont, gm::kErrorTextFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "error_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_default_frame(game, fonts);
    test_messages(game, fonts);
    test_plate(game);
    test_fade_lut(game);
    test_negatives(game);
    if (failures != 0) {
        std::cerr << "error_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "error_render: ok (" << checks << " checks)\n";
    return 0;
}
