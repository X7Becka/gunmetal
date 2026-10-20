#include "render/hud_player.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

#include "core/path.h"

namespace gm {
namespace {

constexpr std::uint8_t kHudBackgroundIndex = 0;

bool read_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "hud: cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "hud: read failed: " + path.string();
        return false;
    }
    return true;
}

/// ``WEAPON8.SOF`` -> letters ``weapon``, id 8; ``Default.sof`` -> id 0
/// (the fallback film is loaded with index 0 at ``0x49597c``).  This is the
/// ``%s%ld`` split of ``film_set`` (``0x49590f``) and ``gm/sof.py:film_id``.
struct FilmName {
    std::string stem;
    std::string letters;
    std::uint64_t id = 0;
    bool has_id = false;
};

bool split_name(std::string_view raw, FilmName& out, std::string& error) {
    const std::size_t slash = raw.find_last_of("/\\");
    std::string_view base = slash == std::string_view::npos ? raw : raw.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        base = base.substr(0, dot);
    }
    if (base.empty()) {
        error = "hud: empty film name";
        return false;
    }

    std::string digits;
    out = FilmName{};
    for (const char c : base) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isdigit(u) != 0) {
            digits.push_back(c);
        } else if (std::isalpha(u) != 0) {
            out.letters.push_back(static_cast<char>(std::tolower(u)));
        } else {
            error = "hud: invalid character in film name '" + std::string(raw) + "'";
            return false;
        }
    }
    if (out.letters.empty()) {
        error = "hud: film name '" + std::string(raw) + "' has no letters";
        return false;
    }
    out.stem = std::string(base);

    if (!digits.empty()) {
        if (digits.size() > 9) {
            error = "hud: numeric suffix of '" + std::string(raw) + "' is too long";
            return false;
        }
        for (const char c : digits) {
            out.id = out.id * 10 + static_cast<std::uint64_t>(c - '0');
        }
        out.has_id = true;
    } else if (out.letters == "default") {
        out.id = 0;
        out.has_id = true;
    }
    if (!out.has_id) {
        error = "hud: film name '" + std::string(raw) + "' has no numeric suffix";
        return false;
    }
    return true;
}

/// The record on physical line *id* of the index (blank lines shift nothing in
/// the shipped files, but the engine skips lines, so compare line numbers).
const FdxEntry* entry_on_line(const Fdx& fdx, std::uint64_t line) {
    for (const auto& entry : fdx.entries) {
        if (entry.line == line) {
            return &entry;
        }
    }
    return nullptr;
}

bool validate(const HudFilm& film, std::string& error) {
    if (film.fps == 0) {
        error = "hud: film has fps 0";
        return false;
    }
    if (film.fps > kFdxMaxFps) {
        error = "hud: fps " + std::to_string(film.fps) + " exceeds " +
                std::to_string(kFdxMaxFps);
        return false;
    }
    if (film.count == 0) {
        error = "hud: film has count 0";
        return false;
    }
    if (static_cast<std::uint64_t>(film.count) > film.sof.tiles()) {
        error = "hud: count " + std::to_string(film.count) + " outside film (" +
                std::to_string(film.sof.tiles()) + " tiles)";
        return false;
    }
    return true;
}

} // namespace

std::optional<HudFilm> load_hud_film(const std::filesystem::path& game,
                                     const std::string& name, std::string& error) {
    error.clear();
    if (name.empty()) {
        error = "hud: empty film name";
        return std::nullopt;
    }
    if (name.size() > kHudMaxNameLength) {
        error = "hud: film name is " + std::to_string(name.size()) + " bytes, limit " +
                std::to_string(kHudMaxNameLength);
        return std::nullopt;
    }

    FilmName parsed;
    if (!split_name(name, parsed, error)) {
        return std::nullopt;
    }

    // ``Film\%s.SOF`` (0x49fc19).  A caller may also pass a path; the suffix
    // split above only looks at the basename, exactly like the engine.
    const bool has_separator = name.find_first_of("/\\") != std::string::npos;
    std::string relative = has_separator ? name : "Film/" + parsed.stem + ".sof";
    if (relative.size() < 4 ||
        !iequals_ascii(std::string_view(relative).substr(relative.size() - 4), ".sof")) {
        relative += ".sof";
    }
    const auto film_path = resolve_ci(game, relative);
    if (!film_path) {
        error = "hud: film not found: " + (game / relative).string();
        return std::nullopt;
    }

    // Sibling index: letters only, lower-cased; ``.fdx`` first, legacy ``.idx``
    // second (gm/sof.py:find_fdx).  The engine always formats ``FILM\%s.fdx``.
    std::optional<std::filesystem::path> fdx_path;
    for (const char* ext : {".fdx", ".idx"}) {
        const auto candidate = resolve_ci(film_path->parent_path(), parsed.letters + ext);
        if (candidate) {
            fdx_path = candidate;
            break;
        }
    }
    if (!fdx_path) {
        error = "hud: no .fdx index for " + film_path->filename().string() + " (expected " +
                parsed.letters + ".fdx)";
        return std::nullopt;
    }

    std::vector<std::uint8_t> sof_bytes;
    if (!read_bytes(*film_path, sof_bytes, error)) {
        return std::nullopt;
    }
    auto sof = parse_sof(sof_bytes, error);
    if (!sof) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> fdx_bytes;
    if (!read_bytes(*fdx_path, fdx_bytes, error)) {
        return std::nullopt;
    }
    auto fdx = parse_fdx(fdx_bytes, error);
    if (!fdx) {
        return std::nullopt;
    }

    const FdxEntry* entry = entry_on_line(*fdx, parsed.id);
    if (entry == nullptr) {
        error = "hud: line " + std::to_string(parsed.id) + " outside " +
                fdx_path->filename().string() + " (" + std::to_string(fdx->entries.size()) +
                " records)";
        return std::nullopt;
    }

    HudFilm film;
    const std::uint32_t count = entry->count; // copy before the Fdx is moved
    const std::uint32_t fps = entry->fps;
    film.sof = std::move(*sof);
    film.fdx = std::move(*fdx);
    film.line = static_cast<int>(parsed.id);
    film.count = count;
    film.fps = fps;
    film.start_frame = 1; // HUD loader 0x4958d0 passes start_frame = 1
    film.loop = true;     // and loop = 1
    if (!validate(film, error)) {
        return std::nullopt;
    }
    return film;
}

std::optional<HudFrame> frame_at(const HudFilm& film, std::uint64_t tick,
                                 std::string& error) {
    error.clear();
    if (!validate(film, error)) {
        return std::nullopt;
    }
    if (tick > std::numeric_limits<std::uint64_t>::max() / film.fps) {
        error = "hud: tick " + std::to_string(tick) + " overflows at fps " +
                std::to_string(film.fps);
        return std::nullopt;
    }

    const std::uint64_t scaled = tick * film.fps;
    const std::uint64_t elapsed = scaled / kHudTicksPerSecond;
    if (elapsed > std::numeric_limits<std::uint64_t>::max() - film.start_frame) {
        error = "hud: tick " + std::to_string(tick) + " overflows with start_frame " +
                std::to_string(film.start_frame);
        return std::nullopt;
    }
    const std::uint64_t position = elapsed + film.start_frame;

    HudFrame out;
    if (film.loop) {
        out.frame = static_cast<std::size_t>(position % film.count);
        out.ended = false;
    } else if (position >= film.count) {
        out.frame = static_cast<std::size_t>(film.count - 1);
        out.ended = true;
    } else {
        out.frame = static_cast<std::size_t>(position);
        out.ended = false;
    }
    return out;
}

bool draw(const HudFilm& film, RenderResult& target,
          const std::vector<std::uint8_t>& palette, int x, int y, bool transparent,
          int scale, double opacity, std::uint64_t tick, std::string& error) {
    error.clear();
    if (x < 0 || y < 0) {
        error = "hud: negative origin (" + std::to_string(x) + "," + std::to_string(y) +
                ")";
        return false;
    }

    const auto current = frame_at(film, tick, error);
    if (!current) {
        return false;
    }

    HudLayer layer;
    layer.x = x;
    layer.y = y;
    layer.transparent = transparent;
    layer.transparent_index = kHudBackgroundIndex;
    layer.opacity = opacity;
    layer.scale = scale;
    return blit_indexed(target, film.sof, current->frame, palette, layer, error);
}

} // namespace gm
