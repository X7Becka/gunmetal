// World census over the shipped levels (ROADMAP M6.5.4): an independent
// recount of the whole installation through the C++ readers.
//
// Every `.sbt`/`.sbw` pair of the installation is loaded with the same readers
// the engine uses — `parse_sbt`, `parse_sbw`, `WorldState::build` — and the
// totals are printed level by level and summed.  This test does not consult
// the Python oracle at run time; the numbers it pins are the ones the ROADMAP
// claims (1779 doors / 3964 switches / 45 lifts) and the `.sbw` corpus totals
// from `docs/AUDIT_NUMBERS.md` (28 192 objects / 235 069 polygons /
// 357 818 vertices).
//
// Classification (see `docs/WORLD_CENSUS.md` for the exact definitions):
//
//   * hard checks — the counted number must equal the reference; a mismatch
//     prints `MISMATCH` with the delta and fails the run;
//   * soft checks — a derived counter whose exact value is not part of the
//     M6.5.4 claim; drift prints `WARN` with the delta, bumps `warnings` and
//     does not fail.  These are already hard-pinned by CTest `doors_full`; the
//     census keeps them as diagnostics so the primary numbers stay the gate.
//
// Two full passes run over the corpus and must reproduce every row: the census
// is deterministic.  The table is also re-summed and compared with the running
// totals, so the printed per-level rows and the headline cannot diverge.
#include "world/doors.h"

#include "res/sbt.h"
#include "res/sbw.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_warnings = 0;
long long g_soft_delta = 0; // sum of the signed deltas of the WARN lines

void fail(const char* what, std::size_t got, std::size_t want) {
    const long long delta = static_cast<long long>(got) - static_cast<long long>(want);
    std::printf("world_census: MISMATCH %s got=%zu expected=%zu delta=%+lld\n", what, got,
                want, delta);
    ++g_failures;
}

/// Hard check: the reference is part of the M6.5.4 claim and must match.
void expect(const char* what, std::size_t got, std::size_t want) {
    if (got == want) {
        return;
    }
    fail(what, got, want);
}

/// Soft check: a derived counter.  Drift is reported, never fatal.
void expect_soft(const char* what, std::size_t got, std::size_t reference) {
    if (got == reference) {
        return;
    }
    const long long delta = static_cast<long long>(got) - static_cast<long long>(reference);
    g_soft_delta += delta;
    ++g_warnings;
    std::printf("world_census: WARN %s got=%zu reference=%zu delta=%+lld\n", what, got,
                reference, delta);
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// One level, or — with an empty `name` — the corpus total.  Every field is
/// a plain counter so that the two passes can be compared field by field.
struct Row {
    std::string name;
    std::size_t sectors = 0;
    std::size_t objects = 0;
    std::size_t polygons = 0;
    std::size_t vertices = 0;
    std::size_t doors = 0;
    std::size_t switches = 0;
    std::size_t lifts = 0;
    std::size_t bound = 0;          // doors with a valid (sector, object)
    std::size_t distinct = 0;       // distinct (sector, object) of the doors
    std::size_t sliding = 0;
    std::size_t linked = 0;         // ids[3] != 0
    std::size_t rotating = 0;       // non-zero angle operand
    std::size_t lift_stops = 0;
    std::size_t door_kind1 = 0;
    std::size_t door_kind2 = 0;
    std::size_t door_kind3 = 0;
    std::size_t switch_world = 0;   // kind 0 (operands are a world position)
    std::size_t switch_aim = 0;     // kind 0x101
    std::size_t switch_blast = 0;   // kind 0x102 / 0x103
    std::size_t switch_exact = 0;   // kind 0x104
    std::size_t switch_other = 0;   // anything else (none in the shipped data)
    std::size_t bad_records = 0;
    std::size_t bad_bindings = 0;
    std::size_t script_doors = 0;   // built from ADD_DOOR, not a DOOR record
    std::size_t script_switches = 0;
    std::size_t script_lifts = 0;
    std::size_t record_doors = 0;   // built from a DOOR/type-18 record
    std::size_t record_switches = 0;
    std::size_t record_lifts = 0;
};

/// All counters except the name, added into *into*.
void accumulate(Row& into, const Row& row) {
    into.sectors += row.sectors;
    into.objects += row.objects;
    into.polygons += row.polygons;
    into.vertices += row.vertices;
    into.doors += row.doors;
    into.switches += row.switches;
    into.lifts += row.lifts;
    into.bound += row.bound;
    into.distinct += row.distinct;
    into.sliding += row.sliding;
    into.linked += row.linked;
    into.rotating += row.rotating;
    into.lift_stops += row.lift_stops;
    into.door_kind1 += row.door_kind1;
    into.door_kind2 += row.door_kind2;
    into.door_kind3 += row.door_kind3;
    into.switch_world += row.switch_world;
    into.switch_aim += row.switch_aim;
    into.switch_blast += row.switch_blast;
    into.switch_exact += row.switch_exact;
    into.switch_other += row.switch_other;
    into.bad_records += row.bad_records;
    into.bad_bindings += row.bad_bindings;
    into.script_doors += row.script_doors;
    into.script_switches += row.script_switches;
    into.script_lifts += row.script_lifts;
    into.record_doors += row.record_doors;
    into.record_switches += row.record_switches;
    into.record_lifts += row.record_lifts;
}

/// True when every counter of *a* equals *b* (the name is ignored).
bool same_counters(const Row& a, const Row& b) {
    return a.sectors == b.sectors && a.objects == b.objects && a.polygons == b.polygons &&
           a.vertices == b.vertices && a.doors == b.doors && a.switches == b.switches &&
           a.lifts == b.lifts && a.bound == b.bound && a.distinct == b.distinct &&
           a.sliding == b.sliding && a.linked == b.linked && a.rotating == b.rotating &&
           a.lift_stops == b.lift_stops && a.door_kind1 == b.door_kind1 &&
           a.door_kind2 == b.door_kind2 && a.door_kind3 == b.door_kind3 &&
           a.switch_world == b.switch_world && a.switch_aim == b.switch_aim &&
           a.switch_blast == b.switch_blast && a.switch_exact == b.switch_exact &&
           a.switch_other == b.switch_other && a.bad_records == b.bad_records &&
           a.bad_bindings == b.bad_bindings && a.script_doors == b.script_doors &&
           a.script_switches == b.script_switches && a.script_lifts == b.script_lifts &&
           a.record_doors == b.record_doors && a.record_switches == b.record_switches &&
           a.record_lifts == b.record_lifts;
}

/// Load one pair and fill *out*.  A malformed file or an inconsistent `.sbw`
/// leaves *error* set and returns false.
bool census_level(const fs::path& sbt_path, const fs::path& sbw_path, Row& out,
                  std::string& error) {
    out = Row{};
    out.name = sbt_path.stem().string();

    const std::vector<std::uint8_t> sbt_bytes = read_file(sbt_path);
    if (sbt_bytes.empty()) {
        error = "empty .sbt";
        return false;
    }
    const std::vector<std::uint8_t> sbw_bytes = read_file(sbw_path);
    if (sbw_bytes.empty()) {
        error = "empty .sbw";
        return false;
    }
    const auto sbt = gm::parse_sbt(sbt_bytes, error);
    if (!sbt) {
        return false;
    }
    const auto sbw = gm::parse_sbw(sbw_bytes, error);
    if (!sbw) {
        return false;
    }

    // `.sbw` side: the census counts what the loader materialised, not what a
    // header field claims.  The two must agree, and the cached totals of the
    // parser (`total_vertices`/`total_polygons`, which `sbw_transform` pins)
    // must equal the per-object sums.
    out.sectors = sbw->header.sectors.size();
    std::size_t objects = 0;
    for (const gm::SbwSectorRef& sector : sbw->header.sectors) {
        objects += sector.objects.size();
    }
    std::size_t polygons = 0;
    std::size_t vertices = 0;
    for (const gm::SbwGeometry& geometry : sbw->geometry) {
        polygons += geometry.polys.size();
        vertices += geometry.vertices.size();
    }
    if (sbw->header.object_count != objects || sbw->geometry.size() != objects ||
        polygons != sbw->total_polygons || vertices != sbw->total_vertices) {
        error = "inconsistent .sbw totals";
        return false;
    }
    out.objects = objects;
    out.polygons = polygons;
    out.vertices = vertices;

    // World side: the shipped levels carry no DOOR/SWITCH/LIFT records, so
    // this is the SCRIPT path of `WorldState::build` (docs/DOORS_CPP.md §1.1).
    gm::WorldState state;
    std::string build_error;
    if (!state.build(*sbt, *sbw, &build_error)) {
        error = "WorldState::build: " + build_error;
        return false;
    }
    const gm::WorldStats& stats = state.stats();
    out.doors = stats.doors;
    out.switches = stats.switches;
    out.lifts = stats.lifts;
    out.bound = stats.doors_bound;
    out.distinct = stats.doors_distinct_objects;
    out.sliding = stats.doors_sliding;
    out.linked = stats.doors_linked;
    out.bad_records = stats.bad_records;
    out.bad_bindings = stats.bad_bindings;
    out.script_doors = stats.script_doors;
    out.script_switches = stats.script_switches;
    out.script_lifts = stats.script_lifts;
    out.record_doors = stats.record_doors;
    out.record_switches = stats.record_switches;
    out.record_lifts = stats.record_lifts;

    for (const gm::Door& door : state.doors()) {
        if (gm::door_rotation_axis(door) >= 0) {
            ++out.rotating;
        }
        switch (door.kind) {
            case 1:
                ++out.door_kind1;
                break;
            case 2:
                ++out.door_kind2;
                break;
            case 3:
                ++out.door_kind3;
                break;
            default:
                break;
        }
    }
    for (const gm::Switch& sw : state.switches()) {
        if (sw.kind == 0) {
            ++out.switch_world;
        } else if (sw.kind == 0x101) {
            ++out.switch_aim;
        } else if (sw.kind == 0x102 || sw.kind == 0x103) {
            ++out.switch_blast;
        } else if (sw.kind == 0x104) {
            ++out.switch_exact;
        } else {
            ++out.switch_other;
        }
    }
    for (const gm::Lift& lift : state.lifts()) {
        out.lift_stops += lift.stops.size();
    }
    return true;
}

void print_row(const Row& row) {
    std::printf(
        "world_census: level=%s sectors=%zu objects=%zu polygons=%zu vertices=%zu "
        "doors=%zu switches=%zu lifts=%zu bound=%zu sliding=%zu linked=%zu "
        "rotating=%zu stops=%zu\n",
        row.name.c_str(), row.sectors, row.objects, row.polygons, row.vertices, row.doors,
        row.switches, row.lifts, row.bound, row.sliding, row.linked, row.rotating,
        row.lift_stops);
}

/// One full sweep over the installation, in sorted file order.  Rows are
/// appended to *rows* and the running totals to *total*.  `verbose` prints the
/// per-level table (only the first pass does).
bool run_census(const fs::path& game, std::vector<Row>& rows, Row& total, bool verbose,
                std::string& error) {
    rows.clear();
    total = Row{};

    std::vector<fs::path> sbt_files;
    std::vector<std::string> sbw_stems;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(game, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext == ".sbt") {
            sbt_files.push_back(entry.path());
        } else if (ext == ".sbw") {
            sbw_stems.push_back(entry.path().stem().string());
        }
    }
    if (ec) {
        error = "cannot list " + game.string() + ": " + ec.message();
        return false;
    }
    std::sort(sbt_files.begin(), sbt_files.end());
    std::sort(sbw_stems.begin(), sbw_stems.end());

    // Every `.sbw` must have an `.sbt`: otherwise the sweep silently skips a
    // world and the totals shrink.
    std::vector<std::string> sbt_stems;
    sbt_stems.reserve(sbt_files.size());
    for (const fs::path& path : sbt_files) {
        sbt_stems.push_back(path.stem().string());
    }
    if (sbt_stems != sbw_stems) {
        error = "the .sbt and .sbw sets differ";
        return false;
    }

    for (const fs::path& sbt_path : sbt_files) {
        const fs::path sbw_path = sbt_path.parent_path() / (sbt_path.stem().string() + ".sbw");
        Row row;
        std::string level_error;
        if (!census_level(sbt_path, sbw_path, row, level_error)) {
            error = row.name + ": " + level_error;
            return false;
        }
        if (row.bound != row.doors) {
            std::printf("world_census: MISMATCH %s doors_bound got=%zu expected=%zu delta=%+lld\n",
                        row.name.c_str(), row.bound, row.doors,
                        static_cast<long long>(row.bound) - static_cast<long long>(row.doors));
            ++g_failures;
        }
        if (verbose) {
            print_row(row);
        }
        accumulate(total, row);
        rows.push_back(std::move(row));
    }
    return true;
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        game = env;
    }
    std::error_code ec;
    if (!fs::is_directory(game, ec) || !fs::exists(game / "level0.sbt", ec)) {
        std::printf("world_census: game not installed (%s), skipped\n", game.string().c_str());
        return 0;
    }

    // --- pass 1: the census table ------------------------------------------
    const auto first_start = std::chrono::steady_clock::now();
    std::vector<Row> rows;
    Row total;
    std::string error;
    if (!run_census(game, rows, total, true, error)) {
        std::fprintf(stderr, "world_census: %s\n", error.c_str());
        return 1;
    }
    const double first_ms = elapsed_ms(first_start);

    // The headline is the ROADMAP M6.5.4 claim plus the `.sbw` corpus totals.
    expect("levels", rows.size(), 36);
    expect("objects", total.objects, 28192);
    expect("polygons", total.polygons, 235069);
    expect("vertices", total.vertices, 357818);
    expect("doors", total.doors, 1779);
    expect("switches", total.switches, 3964);
    expect("lifts", total.lifts, 45);

    // A malformed shipped record or a door that does not bind is a hard error:
    // the whole point of the corpus pin is that all 36 levels are clean.
    expect("bad_records", total.bad_records, 0);
    expect("bad_bindings", total.bad_bindings, 0);

    // Derived counters (docs/DOORS_CPP.md §5.3, docs/FORMATS.md §12.4 and the
    // `tools/re/doors.py` oracle).  Reported, not gated.
    expect_soft("doors_bound", total.bound, 1779);
    expect_soft("doors_distinct_objects", total.distinct, 1779);
    expect_soft("doors_sliding", total.sliding, 1534);
    expect_soft("doors_linked", total.linked, 200);
    expect_soft("doors_rotating", total.rotating, 357);
    expect_soft("lift_stops", total.lift_stops, 151);
    expect_soft("door_kind1", total.door_kind1, 878);
    expect_soft("door_kind2", total.door_kind2, 350);
    expect_soft("door_kind3", total.door_kind3, 551);
    expect_soft("switch_world", total.switch_world, 531);
    expect_soft("switch_aim", total.switch_aim, 1855);
    expect_soft("switch_blast", total.switch_blast, 1198);
    expect_soft("switch_exact", total.switch_exact, 380);
    expect_soft("switch_other", total.switch_other, 0);

    // The printed table must add up to the printed headline.
    Row table_sum;
    for (const Row& row : rows) {
        accumulate(table_sum, row);
    }
    if (rows.size() != 0 && !same_counters(table_sum, total)) {
        std::printf("world_census: MISMATCH table_sum vs total (per-level rows do not add up)\n");
        ++g_failures;
    }

    std::printf(
        "world_census: levels=%zu objects=%zu polygons=%zu vertices=%zu doors=%zu "
        "switches=%zu lifts=%zu\n",
        rows.size(), total.objects, total.polygons, total.vertices, total.doors,
        total.switches, total.lifts);
    std::printf(
        "world_census: bound=%zu distinct=%zu sliding=%zu linked=%zu rotating=%zu "
        "lift_stops=%zu door_kind=1:%zu,2:%zu,3:%zu\n",
        total.bound, total.distinct, total.sliding, total.linked, total.rotating,
        total.lift_stops, total.door_kind1, total.door_kind2, total.door_kind3);
    std::printf(
        "world_census: switches kind 0:%zu 0x101:%zu 0x102/3:%zu 0x104:%zu other:%zu; "
        "source script=%zu/%zu/%zu record=%zu/%zu/%zu\n",
        total.switch_world, total.switch_aim, total.switch_blast, total.switch_exact,
        total.switch_other, total.script_doors, total.script_switches, total.script_lifts,
        total.record_doors, total.record_switches, total.record_lifts);

    // --- pass 2: determinism ------------------------------------------------
    const auto second_start = std::chrono::steady_clock::now();
    std::vector<Row> again;
    Row total_again;
    std::string again_error;
    if (!run_census(game, again, total_again, false, again_error)) {
        std::fprintf(stderr, "world_census: second pass: %s\n", again_error.c_str());
        return 1;
    }
    const double second_ms = elapsed_ms(second_start);

    if (again.size() != rows.size()) {
        std::printf("world_census: MISMATCH determinism levels got=%zu expected=%zu\n",
                    again.size(), rows.size());
        ++g_failures;
    } else {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!same_counters(rows[i], again[i])) {
                std::printf("world_census: MISMATCH determinism on level %s\n",
                            rows[i].name.c_str());
                ++g_failures;
                break;
            }
        }
    }
    if (!same_counters(total, total_again)) {
        std::printf("world_census: MISMATCH determinism on the corpus total\n");
        ++g_failures;
    }

    std::printf("world_census: timing pass1=%.0f ms pass2=%.0f ms\n", first_ms, second_ms);

    if (g_failures != 0) {
        std::fprintf(stderr, "world_census: %d failure(s), %d warning(s)\n", g_failures,
                     g_warnings);
        return 1;
    }
    std::printf("world_census: ok (%d warning(s), soft delta %+lld)\n", g_warnings,
                g_soft_delta);
    return 0;
}
