#include "ui/briefing.h"

#include "core/path.h"
#include "res/sml.h"
#include "ui/sml_pager.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gm {
namespace {

namespace fs = std::filesystem;

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string trimmed(std::string_view text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && is_space(text[first])) {
        ++first;
    }
    while (last > first && is_space(text[last - 1])) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

bool has_sme_extension(const fs::path& path) {
    return lowered(path.extension().string()) == ".sme";
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read error on " + path.string();
        return false;
    }
    return true;
}

/// `game/SML/brief`, resolved case-insensitively (the engine's loader walks the
/// same names without regard to case, `0x497310`).
fs::path brief_directory(const fs::path& game) {
    const auto resolved = resolve_ci(game, "SML/brief");
    return resolved ? *resolved : fs::path{};
}

/// Flatten one pager page: a `y` -> text map preserves the run order of every
/// baseline (`<c>` columns merge into one line in `text_layout`).
BriefingPage make_page(const LayoutPage& page, const std::vector<LayoutRun>& runs,
                       std::size_t& run_index) {
    BriefingPage out;
    out.index = page.index;
    out.lines = static_cast<int>(page.lines.size());
    out.text.reserve(page.lines.size());

    std::map<int, std::string> by_y;
    std::size_t index = run_index;
    for (; index < runs.size(); ++index) {
        const LayoutRun& run = runs[index];
        if (page.end != 0 && run.first >= page.end) {
            break;
        }
        // Characters of the run, exactly what `pager_render` counts: spaces
        // included, `<k>`/`<s>` already expanded by `text_layout`.
        out.glyphs += static_cast<int>(run.text.size());
        by_y[run.y] += run.text;
    }
    run_index = index;

    for (const LayoutLine& line : page.lines) {
        const auto hit = by_y.find(line.y);
        out.text.push_back(hit == by_y.end() ? std::string{} : trimmed(hit->second));
    }
    return out;
}

} // namespace

int Briefing::page_count() const {
    return static_cast<int>(pages.size());
}

int Briefing::line_count() const {
    int count = 0;
    for (const BriefingPage& page : pages) {
        count += page.lines;
    }
    return count;
}

std::string Briefing::plain_text() const {
    std::string out;
    bool first = true;
    for (const BriefingPage& page : pages) {
        for (const std::string& line : page.text) {
            if (!first) {
                out.push_back('\n');
            }
            first = false;
            out += line;
        }
    }
    return out;
}

bool load_briefing(const fs::path& game, const std::string& level, Briefing& out,
                   std::string* error) {
    out = Briefing{};
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    std::error_code ec;
    if (game.empty() || !fs::is_directory(game, ec)) {
        return fail("briefing: game directory not found: " + game.string());
    }
    const fs::path directory = brief_directory(game);
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        return fail("briefing: no SML/brief directory under " + game.string());
    }

    std::string name = trimmed(level);
    if (name.empty()) {
        return fail("briefing: empty level name");
    }
    if (name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        return fail("briefing: level must be a file name, not a path: \"" + level + "\"");
    }
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        if (lowered(name.substr(dot)) != ".sme") {
            return fail("briefing: not a .sme briefing: \"" + level + "\"");
        }
        name.erase(dot);
        if (name.empty()) {
            return fail("briefing: empty level name");
        }
    }

    // The installation is case-insensitive on Windows and can be either way on
    // Linux, so scan instead of joining one spelling.  Sorting makes a
    // duplicate stem (impossible in the shipment) deterministic.
    std::vector<fs::path> matches;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file(ec) || !has_sme_extension(path)) {
            continue;
        }
        if (lowered(path.stem().string()) == lowered(name)) {
            matches.push_back(path);
        }
    }
    if (ec) {
        return fail("briefing: cannot scan " + directory.string() + ": " + ec.message());
    }
    if (matches.empty()) {
        return fail("briefing: file not found: " + name + ".SME under " +
                    directory.string());
    }
    std::sort(matches.begin(), matches.end());
    const fs::path& path = matches.front();

    std::vector<std::uint8_t> bytes;
    std::string read_error;
    if (!read_file(path, bytes, read_error)) {
        return fail("briefing: " + read_error);
    }
    if (bytes.empty()) {
        return fail("briefing: empty file: " + path.string());
    }

    // Validate before the pager so undecodable bytes give the SML reason (and
    // do not depend on font atlases being present).
    std::string sml_error;
    auto document = parse_sml(bytes, sml_error);
    if (!document) {
        return fail("briefing: " + path.string() + ": " + sml_error);
    }

    SmlPager pager(game);
    std::string pager_error;
    if (!pager.open(*document, &pager_error)) {
        return fail("briefing: " + path.string() + ": " + pager_error);
    }

    std::vector<BriefingPage> pages;
    pages.reserve(pager.layout().pages.size());
    std::size_t run_index = 0;
    for (const LayoutPage& page : pager.layout().pages) {
        pages.push_back(make_page(page, pager.layout().runs, run_index));
    }

    Briefing loaded;
    loaded.source = path.string();
    loaded.level = lowered(name);
    loaded.pages = std::move(pages);
    if (loaded.line_count() == 0) {
        return fail("briefing: no text: " + path.string());
    }
    out = std::move(loaded);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::vector<std::string> briefing_levels(const fs::path& game) {
    std::vector<std::string> levels;
    std::error_code ec;
    if (game.empty() || !fs::is_directory(game, ec)) {
        return levels;
    }
    const fs::path directory = brief_directory(game);
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        return levels;
    }
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file(ec) || !has_sme_extension(path)) {
            continue;
        }
        const std::string stem = lowered(path.stem().string());
        if (!stem.empty()) {
            levels.push_back(stem);
        }
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    return levels;
}

} // namespace gm
