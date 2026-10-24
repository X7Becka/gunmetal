#include "render/cockpit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "render/hud_icons.h"
#include "res/bmp.h"

namespace gm {
namespace {

constexpr int kFrameWidth = kCockpitWidth;
constexpr int kFrameHeight = kCockpitHeight;

/// Icon slots of `0x4926a0`: the code copies to buffer rows `0xaa` (170), the
/// HUD layer offsets the buffer by 8 rows, so the frame rows are 162
/// (docs/HUD_AUDIT.md §2.9, docs/HUD_ICONS.md §3).
constexpr int kIconLeftX = 23;
constexpr int kIconRightX = 201;
constexpr int kIconTop = 162;

/// Buffer row -> frame row for every text/icon position
/// (docs/HUD_AUDIT.md §2.9: `case 3` moves the layer up 8 rows).
constexpr int kCodeRowShift = 8;

/// Throttle fill (`0x492f40`): four rows from (202, threshold), <= 53 px.
constexpr int kThrottleX = 202;
constexpr int kThrottleY = 155;
constexpr int kThrottleRows = 4;
constexpr int kThrottleMax = 53;

/// Damage hexagon bars of the same block: three rows each, <= 17 px
/// (`cmp ...,0x11`), colour by length (docs/HUD_AUDIT.md §2.9).
constexpr int kBarTopX = 148;
constexpr int kBarTopY = 151;
constexpr int kBarBottomX = 148;
constexpr int kBarBottomY = 177;
constexpr int kBarRows = 3;
constexpr int kBarMax = 17;

/// Solid fill colour of the gauges: palette index 243 = pure red (`0xf3`).
constexpr std::uint8_t kFillIndex = 0xF3;

/// Engine font table records the cockpit needs (docs/HUD_COMPOSE.md §2 pt. 1
/// for font 6, docs/TEXT_LAYOUT.md §2.3 for the `NUMFONTS` faces).  Font 6 is
/// missing from `text_layout`'s table, so the extraction below is local to the
/// module; the algorithm (cell grid, buffer coordinates, ink trim) is the one
/// of `0x47fe30`/`0x47fa50` and matches the Python oracle `gm/hud.py`.
struct FaceSpec {
    int number;
    const char* atlas;
    int cell_width;
    int cell_height;
    int first_char;
    int glyph_count;
    int columns;
    int column_pitch;
    int row_pitch;
    int origin_x;
    int origin_y;
    int space_advance;
    int extra_before;
    int extra_after;
    bool ink_trim;
};

constexpr FaceSpec kFaceSpecs[] = {
    // FONTS3, the label face (`push 6` at 0x490279; same arguments as font 7
    // but origin_y = 0 instead of 32).
    {6, "FONTS3", 8, 8, 32, 64, 32, 16, 16, 8, 0, 4, 0, 1, true},
    // NUMFONTS: four copies of the ten digits, fixed cell (raw copy).
    {9, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 8, 6, 8, 0, 1, false},
    {10, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 0, 22, 8, 0, 1, false},
    {11, "NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 8, 32, 8, 0, 1, false},
};

constexpr int kFaceCount =
    static_cast<int>(sizeof(kFaceSpecs) / sizeof(kFaceSpecs[0]));

/// One extracted glyph in the engine's buffer order: `pixels` row 0 is the
/// bottom cell row (the atlas is loaded bottom-up); `draw_text` writes row r at
/// `top + r`, exactly like `0x47faf0`.
struct Glyph {
    int advance = 0;
    int width = 0;
    int cell_height = 0;
    std::vector<std::uint8_t> pixels;
};

struct Face {
    int number = 0;
    int first_char = 0;
    std::vector<Glyph> glyphs;

    const Glyph* glyph(int ch) const {
        const int index = ch - first_char;
        if (index < 0 || index >= static_cast<int>(glyphs.size())) {
            return nullptr;
        }
        return &glyphs[static_cast<std::size_t>(index)];
    }

    int advance_of(int ch) const {
        const Glyph* g = glyph(ch);
        return g != nullptr ? g->advance : 0;
    }
};

void set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

void fail(std::vector<std::uint32_t>& rgba, int& width, int& height,
          std::string* error, std::string text) {
    rgba.clear();
    width = 0;
    height = 0;
    set_error(error, std::move(text));
}

std::string image_error(const std::string& asset, const std::string& text) {
    return "cockpit: " + asset + ": " + text;
}

/// Atlas pixel in *buffer* coordinates: row 0 is the bottom row of the image.
std::uint8_t atlas_pixel(const BmpImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return 0;
    }
    return image.pixels[static_cast<std::size_t>(image.height - 1 - y) *
                            static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(x)];
}

/// Extract the glyphs of *spec* from *image* exactly like `0x47fe30`: the cell
/// grid starts at `(origin_x, origin_y)` in buffer rows, pitch
/// `column_pitch`/`row_pitch`; a trimmed face stores the ink columns only and
/// advances by the ink width, a raw face copies the whole cell and advances by
/// the cell (or by `space_advance` for `' '`).
std::optional<Face> build_face(const BmpImage& image, const FaceSpec& spec,
                               std::string& error) {
    if (image.width <= 0 || image.height <= 0) {
        error = "empty bitmap";
        return std::nullopt;
    }
    const int rows = (spec.glyph_count + spec.columns - 1) / spec.columns;
    const int need_w = spec.origin_x + (spec.columns - 1) * spec.column_pitch +
                       spec.cell_width;
    const int need_h =
        spec.origin_y + (rows - 1) * spec.row_pitch + spec.cell_height;
    if (need_w > image.width || need_h > image.height) {
        error = spec.atlas + std::string(" is ") + std::to_string(image.width) +
                "x" + std::to_string(image.height) + ", font " +
                std::to_string(spec.number) + " needs " + std::to_string(need_w) +
                "x" + std::to_string(need_h);
        return std::nullopt;
    }

    Face face;
    face.number = spec.number;
    face.first_char = spec.first_char;
    face.glyphs.resize(static_cast<std::size_t>(spec.glyph_count));

    for (int i = 0; i < spec.glyph_count; ++i) {
        const int code = spec.first_char + i;
        const int gx = spec.origin_x + (i % spec.columns) * spec.column_pitch;
        const int gy = spec.origin_y + (i / spec.columns) * spec.row_pitch;
        const auto any_ink = [&](int column) {
            for (int row = 0; row < spec.cell_height; ++row) {
                if (atlas_pixel(image, gx + column, gy + row) != 0) {
                    return true;
                }
            }
            return false;
        };

        Glyph& glyph = face.glyphs[static_cast<std::size_t>(i)];
        glyph.cell_height = spec.cell_height;
        int advance = 0;
        if (spec.ink_trim) {
            if (code == ' ') {
                advance = spec.space_advance;
            } else {
                int low = -1;
                int high = -1;
                for (int column = 0; column < spec.cell_width; ++column) {
                    if (any_ink(column)) {
                        if (low < 0) {
                            low = column;
                        }
                        high = column;
                    }
                }
                if (low >= 0) {
                    glyph.width = high - low + 1;
                    advance = glyph.width;
                    glyph.pixels.reserve(static_cast<std::size_t>(glyph.width) *
                                         static_cast<std::size_t>(spec.cell_height));
                    for (int row = 0; row < spec.cell_height; ++row) {
                        for (int column = 0; column < glyph.width; ++column) {
                            glyph.pixels.push_back(
                                atlas_pixel(image, gx + low + column, gy + row));
                        }
                    }
                }
            }
        } else {
            glyph.width = spec.cell_width;
            advance = code == ' ' ? spec.space_advance : spec.cell_width;
            glyph.pixels.reserve(static_cast<std::size_t>(glyph.width) *
                                 static_cast<std::size_t>(spec.cell_height));
            for (int row = 0; row < spec.cell_height; ++row) {
                for (int column = 0; column < glyph.width; ++column) {
                    glyph.pixels.push_back(
                        atlas_pixel(image, gx + column, gy + row));
                }
            }
        }
        glyph.advance = advance + spec.extra_before + spec.extra_after;
    }

    error.clear();
    return face;
}

/// Demand-loaded faces plus the "missing asset" counter: an absent atlas is a
/// skip, never a fatal error (the panel is still worth drawing).
struct FaceSet {
    const Arch* arch = nullptr;
    std::array<std::optional<Face>, kFaceCount> faces;
    std::array<bool, kFaceCount> tried{};
    int missing = 0;

    const Face* get(int number) {
        for (int i = 0; i < kFaceCount; ++i) {
            if (kFaceSpecs[i].number != number) {
                continue;
            }
            if (!tried[static_cast<std::size_t>(i)]) {
                tried[static_cast<std::size_t>(i)] = true;
                std::string error;
                const auto image = arch->image(kFaceSpecs[i].atlas, error);
                if (image) {
                    std::string build_error;
                    faces[static_cast<std::size_t>(i)] =
                        build_face(*image, kFaceSpecs[i], build_error);
                    if (!faces[static_cast<std::size_t>(i)]) {
                        error = build_error;
                    }
                }
                if (!faces[static_cast<std::size_t>(i)]) {
                    ++missing;
                }
            }
            return faces[static_cast<std::size_t>(i)]
                       ? &*faces[static_cast<std::size_t>(i)]
                       : nullptr;
        }
        return nullptr;
    }
};

/// Draw *text* with *face* into the index canvas: cell top-left row *top*, pen
/// starting at *x*; index 0 is transparent.  Writes are clipped to
/// [*clip_left*, *clip_right*) columns, so the compass letters stay in their
/// measured window.  Returns the pixels written.
int draw_text(std::vector<std::uint8_t>& canvas, const Face& face, int x, int top,
              std::string_view text, int clip_left = 0,
              int clip_right = kFrameWidth) {
    int pen = x;
    int written = 0;
    for (const char ch : text) {
        const Glyph* glyph = face.glyph(static_cast<unsigned char>(ch));
        if (glyph == nullptr) {
            continue;
        }
        for (int row = 0; row < glyph->cell_height; ++row) {
            const int y = top + row;
            if (y < 0 || y >= kFrameHeight) {
                continue;
            }
            for (int column = 0; column < glyph->width; ++column) {
                const std::uint8_t index =
                    glyph->pixels[static_cast<std::size_t>(row * glyph->width + column)];
                if (index == 0) {
                    continue;
                }
                const int px = pen + column;
                if (px < clip_left || px >= clip_right) {
                    continue;
                }
                canvas[static_cast<std::size_t>(y) * kFrameWidth +
                       static_cast<std::size_t>(px)] = index;
                ++written;
            }
        }
        pen += glyph->advance;
    }
    return written;
}

/// Copy a decoded BMP into the canvas at (*dx*, *dy*).  *flip* selects the
/// engine row order (`flipV`), *use_key* skips index *key*.  Returns the pixels
/// written (fully clipped source pixels do not count).
int blit(std::vector<std::uint8_t>& canvas, const BmpImage& image, int dx, int dy,
         bool flip, bool use_key, std::uint8_t key) {
    int written = 0;
    for (int row = 0; row < image.height; ++row) {
        const int y = dy + (flip ? image.height - 1 - row : row);
        if (y < 0 || y >= kFrameHeight) {
            continue;
        }
        for (int column = 0; column < image.width; ++column) {
            const int x = dx + column;
            if (x < 0 || x >= kFrameWidth) {
                continue;
            }
            const std::uint8_t index =
                image.pixels[static_cast<std::size_t>(row) *
                                 static_cast<std::size_t>(image.width) +
                             static_cast<std::size_t>(column)];
            if (use_key && index == key) {
                continue;
            }
            canvas[static_cast<std::size_t>(y) * kFrameWidth +
                   static_cast<std::size_t>(x)] = index;
            ++written;
        }
    }
    return written;
}

/// Fill a gauge: *rows* rows from (*x*, *y*), *length* pixels each.  Grows to
/// the right, exactly like the `memset` of `0x492f40`.
int fill_bar(std::vector<std::uint8_t>& canvas, int x, int y, int rows, int length,
             std::uint8_t index) {
    int written = 0;
    for (int row = 0; row < rows; ++row) {
        const int py = y + row;
        if (py < 0 || py >= kFrameHeight) {
            continue;
        }
        for (int i = 0; i < length; ++i) {
            const int px = x + i;
            if (px < 0 || px >= kFrameWidth) {
                continue;
            }
            canvas[static_cast<std::size_t>(py) * kFrameWidth +
                   static_cast<std::size_t>(px)] = index;
            ++written;
        }
    }
    return written;
}

/// Colour of a charge bar by its length (docs/HUD_AUDIT.md §2.9).
std::uint8_t bar_colour(int length) {
    if (length < 6) {
        return 0xF3;
    }
    if (length < 12) {
        return 0x2B;
    }
    return 0xFF;
}

/// `100 - 100*armor/armor_max`, clamped to 0..100 and rounded half-up.
int damage_percent(const CockpitState& state) {
    const int armor_max = std::max(state.armor_max, 1);
    const int armor = std::clamp(state.armor, 0, armor_max);
    const long long scaled = 100LL * armor;
    const long long rounded = (scaled + armor_max / 2) / armor_max;
    const long long percent = 100 - rounded;
    return static_cast<int>(std::clamp<long long>(percent, 0, 100));
}

/// `lround(clamp(fraction, 0, 1) * max)`; a non-finite fraction is 0.
int fraction_length(double fraction, int max) {
    if (!std::isfinite(fraction)) {
        return 0;
    }
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    return static_cast<int>(std::lround(clamped * static_cast<double>(max)));
}

/// The empirical heading tape of docs/HUD_ICONS.md §5 (MODEL): a mark every
/// 22.5 deg in the window, longer marks and a letter on the cardinals.  The
/// ink index is the nearest `GAMEPAL` entry to the green of the oracle frame
/// (`(12,252,12)` -> index 191 `(0,255,0)`); the drawing code was not found.
constexpr int kCompassInk = 191;
constexpr int kCompassShortTop = 2;
constexpr int kCompassShortBottom = 4;
constexpr int kCompassLongTop = 5;
constexpr int kCompassLongBottom = 7;
constexpr int kCompassLetterTop = 9;
constexpr double kCompassStepDeg = 22.5;

int draw_compass(std::vector<std::uint8_t>& canvas, double yaw_deg,
                 const Face* letters) {
    CompassGeometry geometry;
    geometry.yaw_deg = yaw_deg;

    int written = 0;
    for (int step = 0; step < 16; ++step) {
        const double bearing = static_cast<double>(step) * kCompassStepDeg;
        const CompassPose pose = compass_pos(bearing, geometry);
        if (pose.x == kCompassInvalid) {
            return written; // non-finite heading: no tape at all
        }
        if (!compass_in_window(pose.x, geometry)) {
            continue;
        }
        const bool cardinal = (step % 4) == 0;
        const int top = cardinal ? kCompassLongTop : kCompassShortTop;
        const int bottom = cardinal ? kCompassLongBottom : kCompassShortBottom;
        const int width = cardinal ? 2 : 1;
        for (int y = top; y <= bottom; ++y) {
            for (int i = 0; i < width; ++i) {
                const int x = pose.x + i;
                if (x < geometry.window_left || x > geometry.window_right) {
                    continue;
                }
                canvas[static_cast<std::size_t>(y) * kFrameWidth +
                       static_cast<std::size_t>(x)] = kCompassInk;
                ++written;
            }
        }
        if (!cardinal || letters == nullptr) {
            continue;
        }
        static constexpr char kLetters[4] = {'N', 'E', 'S', 'W'};
        const char letter = kLetters[step / 4];
        const int advance = letters->advance_of(static_cast<unsigned char>(letter));
        written += draw_text(canvas, *letters, pose.x - advance / 2,
                             kCompassLetterTop, std::string_view(&letter, 1),
                             geometry.window_left, geometry.window_right + 1);
    }
    return written;
}

/// Text/digit slots in frame rows (`code row - 8`), docs/HUD_AUDIT.md §2.9.
struct TextSlot {
    int x;
    int code_y;
};

constexpr TextSlot kWeaponLabel{23, 162};
constexpr TextSlot kKillLabel{52, 186};
constexpr TextSlot kDamageLabel{141, 150};
constexpr TextSlot kThrottleLabel{205, 159};

/// Digit column of the ammo counter: right-aligned to 72 in 8 px steps
/// (`0x4929f4..0x492a6b`), cell top 163.
int ammo_x(int digits) {
    return 72 - 8 * (digits - 1);
}

/// Damage digits are right-aligned to 156 (`x = 166 - 9*len` in the code,
/// measured as 156 - 8*(len-1) on the frame), cell top 181.
int damage_x(int digits) {
    return 156 - 8 * (digits - 1);
}

} // namespace

bool render_cockpit(const std::filesystem::path& game, const CockpitState& state,
                    CockpitOptions options, std::vector<std::uint32_t>& rgba,
                    int& width, int& height, std::string* error,
                    CockpitStats* stats) {
    if (stats != nullptr) {
        *stats = CockpitStats{};
    }
    if (state.armor_max <= 0) {
        fail(rgba, width, height, error,
             "cockpit: armor_max must be positive (got " +
                 std::to_string(state.armor_max) + ")");
        return false;
    }

    const auto index_path = resolve_ci(game, "FRONTEND/BMP.IDX");
    if (!index_path) {
        fail(rgba, width, height, error,
             "cockpit: no FRONTEND/BMP.IDX under '" + game.string() + "'");
        return false;
    }
    std::string open_error;
    auto arch = Arch::open(*index_path, open_error);
    if (!arch) {
        fail(rgba, width, height, error, "cockpit: " + open_error);
        return false;
    }

    std::string palette_error;
    const auto palette = installed_palette(game, palette_error);
    if (!palette) {
        fail(rgba, width, height, error, "cockpit: palette: " + palette_error);
        return false;
    }

    std::string panel_error;
    const auto panel = arch->image("PANEL", panel_error);
    if (!panel) {
        fail(rgba, width, height, error, image_error("PANEL", panel_error));
        return false;
    }
    if (panel->width != kCockpitPanelWidth ||
        panel->height != kCockpitPanelHeight) {
        fail(rgba, width, height, error,
             "cockpit: PANEL is " + std::to_string(panel->width) + "x" +
                 std::to_string(panel->height) + ", expected 320x78 (the " +
                 std::to_string(kFrameWidth) + "x" + std::to_string(kFrameHeight) +
                 " frame)");
        return false;
    }

    // The whole cockpit is composed as palette indices and converted once, so
    // the layers keep the engine's raw-index order (text is drawn after the
    // panel fill and is not faded, docs/HUD_AUDIT.md §2.9).
    std::vector<std::uint8_t> canvas(
        static_cast<std::size_t>(kFrameWidth) * kFrameHeight, 0);

    int elements = 0;
    int missing = 0;
    int ink = 0;

    const int panel_pixels =
        blit(canvas, *panel, kCockpitPanelX, kCockpitPanelY, true, true, 0);
    if (panel_pixels > 0) {
        ++elements;
    }

    FaceSet faces;
    faces.arch = &*arch;

    if (options.icons) {
        std::string icon_error;
        std::string asset_error;
        const auto weapon_ref =
            weapon_icon(state.weapon_id, false, *arch, &icon_error);
        if (!weapon_ref) {
            ++missing;
        } else if (const auto image = arch->image(weapon_ref->asset, asset_error)) {
            // 0x49295b walks the source rows upwards, so a parsed (top-down)
            // bitmap is the picture the player sees.
            if (blit(canvas, *image, kIconLeftX, kIconTop, false, false, 0) > 0) {
                ++elements;
            }
        } else {
            ++missing;
        }

        icon_error.clear();
        asset_error.clear();
        const auto equipment_ref =
            equipment_icon(state.equipment_id, *arch, &icon_error);
        if (!equipment_ref) {
            ++missing;
        } else if (const auto image = arch->image(equipment_ref->asset, asset_error)) {
            // 0x492cf5 walks the rows downwards, i.e. the equipment icon is the
            // vertical mirror (docs/HUD_ICONS.md §3).
            if (blit(canvas, *image, kIconRightX, kIconTop, true, false, 0) > 0) {
                ++elements;
            }
        } else {
            ++missing;
        }
    }

    // Fills first: the text layer is drawn **after** them and therefore sits
    // on top of the red bars (docs/HUD_COMPOSE.md part II §1: "текст — после
    // заливок").  Gauges are not text, so they are drawn whether or not the
    // numbers are requested.
    const int throttle = fraction_length(state.speed, kThrottleMax);
    if (fill_bar(canvas, kThrottleX, kThrottleY, kThrottleRows, throttle,
                 kFillIndex) > 0) {
        ++elements;
    }

    // The code reads `charge_accumulated (+0x2FA8)` here (hypothesis); the
    // state has no charge, so the bars show the damage percentage (MODEL).
    const int damage = damage_percent(state);
    const int top_length = static_cast<int>(
        std::lround(static_cast<double>(damage) * kBarMax / 100.0));
    if (fill_bar(canvas, kBarTopX, kBarTopY, kBarRows, top_length,
                 bar_colour(top_length)) > 0) {
        ++elements;
    }
    if (fill_bar(canvas, kBarBottomX, kBarBottomY, kBarRows, top_length,
                 bar_colour(top_length)) > 0) {
        ++elements;
    }

    if (options.numbers) {
        const Face* labels = faces.get(6);
        if (labels != nullptr) {
            const bool known_weapon = state.weapon_id >= 0 &&
                                      state.weapon_id < kWeaponIconCount;
            const std::string weapon_name =
                known_weapon
                    ? weapon_icon_table()[static_cast<std::size_t>(state.weapon_id)]
                          .name
                    : std::string("NO WEAPON");
            const auto draw_label = [&](const TextSlot& slot, const std::string& text) {
                const int written =
                    draw_text(canvas, *labels, slot.x, slot.code_y - kCodeRowShift,
                              text);
                ink += written;
                if (written > 0) {
                    ++elements;
                }
            };
            draw_label(kWeaponLabel, weapon_name);
            draw_label(kKillLabel, "KILL");
            draw_label(kDamageLabel, "DAMAGE");
            draw_label(kThrottleLabel, "THROTTLE");
        }

        const int ammo = std::max(state.ammo, 0);
        const std::string ammo_text = std::to_string(ammo);
        if (const Face* digits = faces.get(10)) {
            const int written = draw_text(canvas, *digits, ammo_x(
                                             static_cast<int>(ammo_text.size())),
                                          171 - kCodeRowShift, ammo_text);
            ink += written;
            if (written > 0) {
                ++elements;
            }
        }

        const std::string kills_text =
            std::to_string(std::max(state.kills, 0));
        if (const Face* digits = faces.get(11)) {
            const int written = draw_text(canvas, *digits, 72,
                                          185 - kCodeRowShift, kills_text);
            ink += written;
            if (written > 0) {
                ++elements;
            }
        }

        const std::string damage_text = std::to_string(damage);
        // `0x49270e`: the value turns red (`NUMFONTS` 10) from 70 %.
        if (const Face* digits = faces.get(damage < 70 ? 9 : 10)) {
            const int written =
                draw_text(canvas, *digits,
                          damage_x(static_cast<int>(damage_text.size())),
                          189 - kCodeRowShift, damage_text);
            ink += written;
            if (written > 0) {
                ++elements;
            }
        }
    }

    if (options.compass) {
        const Face* letters = faces.get(6);
        if (draw_compass(canvas, state.yaw_deg, letters) > 0) {
            ++elements;
        }
    }

    missing += faces.missing;

    rgba.resize(static_cast<std::size_t>(kFrameWidth) * kFrameHeight);
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const std::uint8_t index = canvas[i];
        const std::size_t offset = static_cast<std::size_t>(index) * 4;
        const std::uint32_t r = (*palette)[offset + 0];
        const std::uint32_t g = (*palette)[offset + 1];
        const std::uint32_t b = (*palette)[offset + 2];
        rgba[i] = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
    }
    width = kFrameWidth;
    height = kFrameHeight;

    if (stats != nullptr) {
        stats->elements_drawn = elements;
        stats->missing_assets = missing;
        stats->ink_pixels = ink;
        stats->panel_pixels = panel_pixels;
    }
    clear_error(error);
    return true;
}

} // namespace gm
