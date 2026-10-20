#include "res/soe.h"

#include <array>
#include <charconv>
#include <limits>
#include <utility>

namespace gm {
namespace {

// ---------------------------------------------------------------------------
// Record map: the C++ copy of gm/soe.py FIELD_MAP (77 rows: 40 proven,
// 22 hypothesis, 15 unknown).  Keep the order and the strings identical to the
// Python table; docs/SOE.md §3 explains every row.
// ---------------------------------------------------------------------------
const std::array<SoeFieldInfo, 77> kFieldMap = {{
    {0x000, 16, SoeValueKind::Name16, "name", "thing-type name (search key)", SoeStatus::Proven,
     std::string_view{}, std::string_view{}, 1, 0, -1,
     "copy at 0x47cb6c; lookup 0x47cb00", "0x47cb7a, 0x47cb9f, 0x47cb23"},
    {0x010, 8, SoeValueKind::Double, "off_z", "spawn height offset (hypothesis)", SoeStatus::Hypothesis,
     "col_f10", std::string_view{}, 1, 0, 0x58,
     "%lf 0x47cff8", "0x46d0ff, 0x46d10b"},
    {0x018, 8, SoeValueKind::Double, "f18", "unknown (v>=12)", SoeStatus::Unknown,
     "f18", std::string_view{}, 1, 0, 0x60,
     "%lf 0x47d98f", "0x46dd17"},
    {0x020, 8, SoeValueKind::Double, "f20", "unknown (physics/scale?)", SoeStatus::Hypothesis,
     "col_f20", std::string_view{}, 1, 0, 0x18,
     "%lf 0x47cff8", "0x471552, 0x472c42"},
    {0x028, 8, SoeValueKind::Double, "f28", "unknown (physics/scale?)", SoeStatus::Hypothesis,
     "col_f28", std::string_view{}, 1, 0, 0x20,
     "%lf 0x47cff8", "0x471604, 0x47495a"},
    {0x030, 8, SoeValueKind::Double, "radius", "bounding radius (computed)", SoeStatus::Proven,
     "radius/radius_max", std::string_view{}, 1, 0, 0x30,
     "computed at 0x47cf61 / 0x47cfad", "0x47cec7, 0x47cf23"},
    {0x038, 8, SoeValueKind::Double, "radius_x", "ellipsoid radius X", SoeStatus::Proven,
     "radius_x", std::string_view{}, 1, 0, 0x38,
     "0x47ce91 %lf %lf %lf", "0x47ced3"},
    {0x040, 8, SoeValueKind::Double, "radius_y", "ellipsoid radius Y", SoeStatus::Proven,
     "radius_y", std::string_view{}, 1, 0, 0x40,
     "0x47ce91", "0x47cefe"},
    {0x048, 8, SoeValueKind::Double, "radius_z", "ellipsoid radius Z", SoeStatus::Proven,
     "radius_z", std::string_view{}, 1, 0, 0x48,
     "0x47ce91", "0x47cf04"},
    {0x050, 8, SoeValueKind::Double, "f50", "unknown double", SoeStatus::Unknown,
     "f50", std::string_view{}, 1, 0, 0x50,
     "%lf 0x47cfc9", "0x46dce7"},
    {0x058, 8, SoeValueKind::Double, "search_radius",
     "target search radius, double (AI types 2000..9000)", SoeStatus::Proven,
     "f58", std::string_view{}, 1, 0, 0x68,
     "0x473a00; copy 0x46dbeb", "0x473a00, 0x46dbeb"},
    {0x060, 8, SoeValueKind::Double, "f60", "unknown double (not copied to the instance)", SoeStatus::Unknown,
     "f60", std::string_view{}, 1, 0, -1,
     "%lf 0x47cfc9; read with +0x50/+0x58/+0x68 at 0x47cfcf", "0x47cfb3"},
    {0x068, 8, SoeValueKind::Double, "f68", "unknown double (copied to instance +0x70)", SoeStatus::Unknown,
     "f68", std::string_view{}, 1, 0, 0x70,
     "%lf 0x47cfc9", "0x46da94, 0x46daa6"},
    {0x070, 8, SoeValueKind::Double, "f70", "unknown (v>=12)", SoeStatus::Unknown,
     "f70", std::string_view{}, 1, 0, 0x78,
     "%lf 0x47d98f", "0x46dbd9"},
    {0x078, 8, SoeValueKind::Double, "f78", "unknown double", SoeStatus::Unknown,
     "f78", std::string_view{}, 1, 0, 0x90,
     "%lf 0x47d347", "0x46da64"},
    {0x080, 8, SoeValueKind::Double, "f80", "unknown double", SoeStatus::Unknown,
     "f80", std::string_view{}, 1, 0, 0x80,
     "%lf 0x47d347", "0x46deff"},
    {0x088, 8, SoeValueKind::Double, "f88", "unknown double", SoeStatus::Unknown,
     "f88", std::string_view{}, 1, 0, 0x88,
     "%lf 0x47d347", "0x46df0b"},
    {0x090, 8, SoeValueKind::Double, "col_f90", "collision grid/step value", SoeStatus::Hypothesis,
     "col_f90", std::string_view{}, 1, 0, 0x0,
     "%lf 0x47ccf9; *0.000191794 at 0x456f40", "0x456f40, 0x4a2550"},
    {0x098, 8, SoeValueKind::Double, "col_f98", "collision grid/step value", SoeStatus::Hypothesis,
     "col_f98", std::string_view{}, 1, 0, 0x8,
     "%lf 0x47ccf9; *0.000191794", "0x456f40"},
    {0x0a0, 8, SoeValueKind::Double, "col_fa0", "collision grid/step value", SoeStatus::Hypothesis,
     "col_fa0", std::string_view{}, 1, 0, 0x10,
     "%lf 0x47ccf9; *0.000191794", "0x456f40"},
    {0x0a8, 8, SoeValueKind::Double, "f_a8", "unknown (v>=10)", SoeStatus::Unknown,
     "f_a8", std::string_view{}, 1, 0, 0x1e0,
     "%lf 0x47d87f", "0x46d9fe"},
    {0x0b0, 8, SoeValueKind::Double, "f_b0", "unknown (v>=10)", SoeStatus::Unknown,
     "f_b0", std::string_view{}, 1, 0, 0x1e8,
     "%lf 0x47d87f", "0x46da16"},
    {0x0b8, 8, SoeValueKind::Double, "f_b8", "unknown (v>=11)", SoeStatus::Unknown,
     "f_b8", std::string_view{}, 1, 0, 0x1f0,
     "%lf 0x47d87f", "0x46da2e"},
    {0x0c0, 8, SoeValueKind::Double, "f_c0", "unknown (v>=11)", SoeStatus::Unknown,
     "f_c0", std::string_view{}, 1, 0, 0x1f8,
     "%lf 0x47d87f", "0x46da46"},
    {0x0c8, 8, SoeValueKind::Double, "falling_fear", "falling fear", SoeStatus::Proven,
     "falling_fear", std::string_view{}, 1, 0, 0x188,
     "%lf 0x47d565; error 0x4bb838", "0x47d580"},
    {0x0d0, 8, SoeValueKind::Double, "leash_distance", "leash distance/radius", SoeStatus::Hypothesis,
     "f_d0", std::string_view{}, 1, 0, 0x248,
     "v>=13, follows leash location 0x47d9f1", "0x46dad0"},
    {0x0d8, 8, SoeValueKind::Double, "child_x", "child offset X (4 entries, stride 0x18)", SoeStatus::Proven,
     "child[i].c0", std::string_view{}, 4, 24, -1,
     "children block 0x47de02; stride 0x18 at 0x46de48", "0x46d229"},
    {0x0e0, 8, SoeValueKind::Double, "child_y", "child offset Y (4 entries, stride 0x18)", SoeStatus::Proven,
     "child[i].c1", std::string_view{}, 4, 24, -1,
     "children block 0x47de02", "0x46d273"},
    {0x0e8, 8, SoeValueKind::Double, "child_z", "child offset Z (4 entries, stride 0x18)", SoeStatus::Proven,
     "child[i].c2", std::string_view{}, 4, 24, -1,
     "children block 0x47de02", "0x46d264"},
    {0x138, 8, SoeValueKind::Double, "child_r1", "child angle/scale 1 (4 entries, stride 8)", SoeStatus::Proven,
     "child[i].c3", std::string_view{}, 4, 8, -1,
     "children block 0x47de02", "0x46d259"},
    {0x158, 8, SoeValueKind::Double, "child_r2", "child angle/scale 2 (4 entries, stride 8)", SoeStatus::Proven,
     "child[i].c4", std::string_view{}, 4, 8, -1,
     "children block 0x47de02", "0x46d23a"},
    {0x178, 8, SoeValueKind::Double, "child_r3", "child angle/scale 3 (4 entries, stride 8)", SoeStatus::Proven,
     "child[i].c5", std::string_view{}, 4, 8, -1,
     "children block 0x47de02", "0x46d229"},
    {0x198, 4, SoeValueKind::Int, "f198", "spawn guard upper bound (value 10)", SoeStatus::Hypothesis,
     "f198", std::string_view{}, 1, 0, -1,
     "%ld 0x47cc6f; check 0x46d95c", "0x46d95c, 0x83d62c"},
    {0x19c, 4, SoeValueKind::Int, "f19c", "spawn guard lower bound (value 0)", SoeStatus::Hypothesis,
     "f19c", std::string_view{}, 1, 0, -1,
     "%ld 0x47cc6f; check 0x46d950", "0x46d950, 0x83d62c"},
    {0x1a0, 4, SoeValueKind::Int, "collision_kind", "0 none / 1 sphere / 2 ellipsoid", SoeStatus::Proven,
     "collision_shape", std::string_view{}, 1, 0, 0x2e8,
     "0x47ce51..0x47cfad; switch 0x46e282", "0x47cf23, 0x47cfad, 0x46e282"},
    {0x1a4, 4, SoeValueKind::Int, "sob_index", "index into SOB_TABLE (model)", SoeStatus::Proven,
     "class_name", std::string_view{}, 1, 0, 0x2f4,
     "0x47cd57 call 0x47c9b0; store 0x47cd75", "0x46d0a0, 0x46d308, engine/src/res/scene.cpp:resolve_thing_tables"},
    {0x1a8, 4, SoeValueKind::Int, "flags_a", "thing flag word A", SoeStatus::Hypothesis,
     "f1a8", std::string_view{}, 1, 0, 0x318,
     "%ld 0x47cc6f; bits tested 0x1/0x2/0x20/0x20000", "0x46d992, 0x46c96e, 0x473aef"},
    {0x1ac, 4, SoeValueKind::Int, "flags_b", "thing flag word B", SoeStatus::Hypothesis,
     "f1ac", std::string_view{}, 1, 0, 0x31c,
     "%ld 0x47cc6f; bits tested 0x40000/0x1000000/0x40000000", "0x46d99e, 0x471593, 0x473967"},
    {0x1b0, 4, SoeValueKind::Int, "flags_c", "thing flag word C (v>=9)", SoeStatus::Hypothesis,
     "f1b0", std::string_view{}, 1, 0, 0x320,
     "%ld 0x47ccb9; bits tested 0x40000/0x1000000", "0x46d9aa, 0x46d137, 0x471a7c"},
    {0x1b4, 4, SoeValueKind::Int, "movement_type", "movement type 0..18 (0 none, 13 flee, 14 attack, 18 pushable)", SoeStatus::Proven,
     "collision_type", "collision_type", 1, 0, 0x348,
     "0x46c840 table 0x4ba8c8; store 0x47d4e6", "0x46c840, 0x4ba8c8, 0x47093c, 0x472610"},
    {0x1b8, 4, SoeValueKind::Int, "event_low_health", "event when health crosses 50%", SoeStatus::Hypothesis,
     "event_a", std::string_view{}, 1, 0, 0x34c,
     "%ld 0x47d03a; trigger 0x4704a9", "0x4704a9, 0x470482"},
    {0x1bc, 4, SoeValueKind::Int, "event_morale_failure", "morale-failure event", SoeStatus::Proven,
     "morale_failure_event", std::string_view{}, 1, 0, 0x4cc,
     "error 0x4bb7a0; store 0x47d690", "0x47d6b0, 0x470b02"},
    {0x1c0, 4, SoeValueKind::Int, "event_timeout", "time-out event", SoeStatus::Proven,
     "time_out_event", std::string_view{}, 1, 0, 0x334,
     "error 0x4bb738; store 0x47d6f2", "0x47d6f2, 0x470d4b"},
    {0x1c4, 4, SoeValueKind::Int, "event_death", "event on hit-count death (second 10-block string)", SoeStatus::Hypothesis,
     "str_f0", std::string_view{}, 1, 0, 0x2e0,
     "resolve 0x47d4a9; trigger 0x47573a", "0x47573a, 0x475646"},
    {0x1c8, 4, SoeValueKind::Int, "hits", "hit count / toughness (decremented per impact)", SoeStatus::Hypothesis,
     "f1c8", std::string_view{}, 1, 0, 0x2e4,
     "%ld 0x47d347; 0x475615", "0x475615, 0x475624"},
    {0x1cc, 4, SoeValueKind::Int, "target_zone_threshold",
     "target-in-zone threshold, default 200 (not search radius)", SoeStatus::Proven,
     "f1cc", std::string_view{}, 1, 0, 0x2ec,
     "v>=7 0x47d7c0; 0x473fb4", "0x473fb4, 0x47615a"},
    {0x1d0, 4, SoeValueKind::Int, "target_history_offset",
     "target position-history lag (0..10 shipped; clamp 1..0xef)", SoeStatus::Proven,
     "f1d0", std::string_view{}, 1, 0, 0x2f0,
     "v>=7 0x47d7c0; 0x473eed / 0x476523", "0x473eed, 0x476523"},
    {0x1d4, 4, SoeValueKind::Int, "max_health",
     "maximum HP; current=max at spawn, 0 = damage-immune", SoeStatus::Proven,
     "f1d4", std::string_view{}, 1, 0, 0x350,
     "%ld 0x47d03a; copy 0x46dc1b/0x46dc21; damage 0x470469", "0x46dc1b, 0x46dc21, 0x470469"},
    {0x1d8, 4, SoeValueKind::Int, "shield0", "max shield layer 0", SoeStatus::Proven,
     "shield0", std::string_view{}, 1, 0, 0x368,
     "0x4b9640 / error 0x4bb974", "0x470452, 0x47045d"},
    {0x1dc, 4, SoeValueKind::Int, "shield1", "max shield layer 1", SoeStatus::Proven,
     "shield1", std::string_view{}, 1, 0, 0x36c,
     "0x4b9640", "0x45e7cb"},
    {0x1e0, 4, SoeValueKind::Int, "shield2", "max shield layer 2", SoeStatus::Proven,
     "shield2", std::string_view{}, 1, 0, 0x370,
     "0x4b9640", "0x45e7e5"},
    {0x1e4, 4, SoeValueKind::Int, "shield3", "max shield layer 3", SoeStatus::Proven,
     "shield3", std::string_view{}, 1, 0, 0x374,
     "0x4b9640", "0x45e7ff"},
    {0x1e8, 4, SoeValueKind::Int, "expire_ticks", "lifetime ticks (loader stores 0.91 * token)", SoeStatus::Proven,
     "expire_value", std::string_view{}, 1, 0, 0x328,
     "%ld 0x47d24b; scale 0x47d3fb; tick 0x470c9c", "0x470c9c, 0x470cad, 0x4703a0"},
    {0x1ec, 4, SoeValueKind::Int, "event_expire", "expire event", SoeStatus::Proven,
     "expire_event", std::string_view{}, 1, 0, 0x32c,
     "error 0x4bb8b0; store 0x47d2bd; trigger 0x4703f3", "0x4703f3, 0x47d2b8"},
    {0x1f0, 4, SoeValueKind::Int, "event_create", "create event", SoeStatus::Proven,
     "create_event", std::string_view{}, 1, 0, 0x38c,
     "error 0x4bb808; store 0x47d5e4; trigger 0x46e275", "0x46e275, 0x46f6df"},
    {0x1f4, 4, SoeValueKind::Int, "event_damaged", "damaged event", SoeStatus::Proven,
     "damaged_event", std::string_view{}, 1, 0, 0x4d0,
     "error 0x4bb6d4; store 0x47d755; trigger 0x4709a2", "0x470995, 0x4709a2"},
    {0x1f8, 4, SoeValueKind::Int, "event_destroy", "event when thing is removed/destroyed (first 10-block string)", SoeStatus::Hypothesis,
     "str_c8", std::string_view{}, 1, 0, 0x390,
     "resolve 0x47d450; trigger 0x4702e9", "0x4702e9, 0x470338"},
    {0x1fc, 4, SoeValueKind::Int, "sound_id", "sound sample index (loop sound)", SoeStatus::Proven,
     "str_1a4", std::string_view{}, 1, 0, 0x388,
     "0x40b9c0 table 0x4c9c90; store 0x47d506; play 0x4720bf", "0x40b9c0, 0x472080"},
    {0x200, 4, SoeValueKind::Int, "sound_volume", "sound volume (loader stores 0.64 * token)", SoeStatus::Hypothesis,
     "f200", std::string_view{}, 1, 0, 0x384,
     "scale 0x47d50c; play 0x4720bf", "0x4720bf, 0x46b960"},
    {0x204, 4, SoeValueKind::Int, "leash_location", "leash location identifier", SoeStatus::Proven,
     "leash_location", std::string_view{}, 1, 0, -1,
     "error 0x4bb658; resolve 0x47da4f", "0x47da4f, 0x46db49"},
    {0x208, 4, SoeValueKind::Int, "texture_a", "texture id A", SoeStatus::Proven,
     "tex_a", std::string_view{}, 1, 0, 0x378,
     "0x42b81b; store 0x47d1de", "0x42b81b, 0x47d1de"},
    {0x20c, 4, SoeValueKind::Int, "texture_b", "texture id B", SoeStatus::Proven,
     "tex_b", std::string_view{}, 1, 0, 0x37c,
     "0x42b81b; store 0x47d201", "0x42b81b, 0x47d201"},
    {0x210, 4, SoeValueKind::Int, "timer_period", "periodic AI/weapon period (divisor)", SoeStatus::Hypothesis,
     "f210", std::string_view{}, 1, 0, 0x3a4,
     "%ld 0x47d347; 0x473eed / 0x476523", "0x473eed, 0x476523"},
    {0x214, 4, SoeValueKind::Int, "timer_step", "periodic AI/weapon increment (loader stores 0.91 * token)", SoeStatus::Hypothesis,
     "f214", std::string_view{}, 1, 0, 0x3a8,
     "scale 0x47d52f; 0x473f07 / 0x47653d", "0x473f07, 0x47653d"},
    {0x218, 4, SoeValueKind::Int, "weapon_count", "number of weapons (<=8)", SoeStatus::Proven,
     "weapon_count", std::string_view{}, 1, 0, 0x3ac,
     "%ld 0x47dbbb; clamp 0x47dc2c", "0x47dc2c, 0x46dd7d"},
    {0x21c, 4, SoeValueKind::Int, "weapon_id", "weapon identifier (resolved)", SoeStatus::Proven,
     "weapon[i].name", std::string_view{}, 8, 4, -1,
     "0x45fb90 in weapon loop 0x47dd21", "0x47dd21, 0x46ddc6"},
    {0x23c, 4, SoeValueKind::Int, "weapon_p0", "weapon parameter 0 (range?)", SoeStatus::Hypothesis,
     "weapon[i].w0", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; 0x473def/0x473e40", "0x473def, 0x473e40"},
    {0x25c, 4, SoeValueKind::Int, "weapon_p1", "weapon parameter 1 (range?)", SoeStatus::Hypothesis,
     "weapon[i].w1", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; 0x473dfc/0x473e4d", "0x473dfc, 0x473e4d"},
    {0x27c, 4, SoeValueKind::Int, "weapon_p2", "weapon parameter 2 (cooldown; loader stores 0.91 * token)", SoeStatus::Hypothesis,
     "weapon[i].w2", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; scale 0x47dcff; cooldown 0x473ecc", "0x473ec3, 0x47134f"},
    {0x29c, 4, SoeValueKind::Int, "weapon_p3", "weapon parameter 3 (double at runtime)", SoeStatus::Unknown,
     "weapon[i].w3", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; 0x46ddb2", "0x46ddb2"},
    {0x2bc, 4, SoeValueKind::Int, "weapon_p4", "weapon parameter 4 (double at runtime)", SoeStatus::Unknown,
     "weapon[i].w4", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; 0x46ddbb", "0x46ddbb"},
    {0x2dc, 4, SoeValueKind::Int, "weapon_p5", "weapon parameter 5 (double at runtime)", SoeStatus::Unknown,
     "weapon[i].w5", std::string_view{}, 8, 4, -1,
     "%ld 0x47dcaa; 0x46ddc0", "0x46ddc0"},
    {0x2fc, 4, SoeValueKind::Int, "children_count", "number of children (<=4)", SoeStatus::Proven,
     "children_count", std::string_view{}, 1, 0, -1,
     "%ld 0x47dd7f; clamp 0x47ddad", "0x47ddad, 0x46d192"},
    {0x300, 4, SoeValueKind::Int, "child_type", "child thing-type index", SoeStatus::Proven,
     "child[i].name", std::string_view{}, 4, 4, -1,
     "0x47cb00 call 0x47de1a; store 0x47de28", "0x47de1a, 0x46d293"},
    {0x310, 4, SoeValueKind::Int, "child_param", "child extra integer", SoeStatus::Proven,
     "child[i].x", std::string_view{}, 4, 4, -1,
     "%ld 0x47de02", "0x46d3ec"},
    {0x320, 4, SoeValueKind::Int, "path_count", "path-point count (<=32)", SoeStatus::Proven,
     "path_point_count", std::string_view{}, 1, 0, 0x438,
     "%ld 0x47dab5; clamp 0x47daea", "0x47daea, 0x46de66"},
    {0x324, 4, SoeValueKind::Int, "path_point", "path location identifier", SoeStatus::Proven,
     "path_point[i]", std::string_view{}, 32, 4, 0x43c,
     "0x460000 in loop 0x47db3d", "0x47db3d, 0x46de90"},
}};

// ---------------------------------------------------------------------------
// Tokenisation.  Python decodes the file as latin1 and calls str.split(), so
// the separator set is exactly the C0/C1 whitespace of latin1, not the C
// locale's isspace(): 0x09..0x0d, 0x1c..0x1f, 0x20, 0x85, 0xa0.
// ---------------------------------------------------------------------------
constexpr bool py_space(unsigned char c) {
    return c == 0x20 || (c >= 0x09 && c <= 0x0d) ||
           (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0;
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

struct ParseError {
    std::string what;
};

[[noreturn]] void fail(std::string message) { throw ParseError{std::move(message)}; }

std::vector<std::string> tokenize(std::span<const std::uint8_t> data) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < data.size()) {
        while (i < data.size() && py_space(data[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < data.size() && !py_space(data[i])) {
            ++i;
        }
        if (i == start) {
            continue;
        }
        const std::size_t length = i - start;
        if (length > kSoeMaxTokenBytes) {
            fail("token " + std::to_string(tokens.size()) + " is " +
                 std::to_string(length) + " bytes, limit is " +
                 std::to_string(kSoeMaxTokenBytes));
        }
        if (tokens.size() >= kSoeMaxTokens) {
            fail(".soe has more than " + std::to_string(kSoeMaxTokens) + " tokens");
        }
        tokens.emplace_back(reinterpret_cast<const char*>(data.data() + start), length);
    }
    return tokens;
}

// ---------------------------------------------------------------------------
// Scalar conversion.  The grammar comes from fscanf("%ld"/"%lf"), but the
// reference implementation is Python's int()/float(), so the accepted syntax
// is the decimal subset both agree on (no hex, no trailing junk).
// ---------------------------------------------------------------------------
bool parse_int(std::string_view token, std::int64_t& out) {
    if (token.empty()) {
        return false;
    }
    std::size_t i = 0;
    bool negative = false;
    if (token[0] == '+' || token[0] == '-') {
        negative = token[0] == '-';
        ++i;
    }
    if (i == token.size()) {
        return false;
    }
    std::uint64_t magnitude = 0;
    constexpr std::uint64_t limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (limit - digit) / 10) {
            return false;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        if (magnitude > limit + 1) {
            return false;
        }
        out = magnitude == limit + 1
                  ? std::numeric_limits<std::int64_t>::min()
                  : -static_cast<std::int64_t>(magnitude);
    } else {
        if (magnitude > limit) {
            return false;
        }
        out = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool parse_double(std::string_view token, double& out) {
    if (token.empty()) {
        return false;
    }
    // std::from_chars does not accept a leading '+' (Python float does).
    std::string_view body = token;
    if (body.front() == '+') {
        body.remove_prefix(1);
    }
    if (body.empty()) {
        return false;
    }
    double value = 0.0;
    const char* first = body.data();
    const char* last = body.data() + body.size();
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

class Reader {
public:
    explicit Reader(const std::vector<std::string>& tokens) : tokens_(tokens) {}

    bool eof() const { return index_ >= tokens_.size(); }
    std::size_t position() const { return index_; }
    std::size_t size() const { return tokens_.size(); }

    const std::string& str() {
        if (eof()) {
            fail("unexpected end of .soe (expected a string)");
        }
        return tokens_[index_++];
    }

    std::int64_t integer() {
        if (eof()) {
            fail("unexpected end of .soe (expected an integer)");
        }
        std::int64_t value = 0;
        if (!parse_int(tokens_[index_], value)) {
            fail("expected an integer, got \"" + tokens_[index_] + "\" at token " +
                 std::to_string(index_));
        }
        ++index_;
        return value;
    }

    double real() {
        if (eof()) {
            fail("unexpected end of .soe (expected a number)");
        }
        double value = 0.0;
        if (!parse_double(tokens_[index_], value)) {
            fail("expected a number, got \"" + tokens_[index_] + "\" at token " +
                 std::to_string(index_));
        }
        ++index_;
        return value;
    }

private:
    const std::vector<std::string>& tokens_;
    std::size_t index_ = 0;
};

bool split_array_label(std::string_view label, std::string_view prefix,
                       std::size_t& index, std::string_view& sub) {
    if (!label.starts_with(prefix) || label.size() <= prefix.size() ||
        label[prefix.size()] != '[') {
        return false;
    }
    std::size_t i = prefix.size() + 1;
    std::size_t value = 0;
    bool digits = false;
    while (i < label.size() && label[i] >= '0' && label[i] <= '9') {
        digits = true;
        if (value > 10000) {
            return false; // labels are generated locally; keep them bounded
        }
        value = value * 10 + static_cast<std::size_t>(label[i] - '0');
        ++i;
    }
    if (!digits || i >= label.size() || label[i] != ']') {
        return false;
    }
    ++i;
    sub = label.substr(i);
    if (!sub.empty()) {
        if (sub.front() != '.') {
            return false;
        }
        sub.remove_prefix(1);
    }
    index = value;
    return true;
}

const SoeFieldInfo* find_by_name(std::string_view name) {
    for (const auto& info : kFieldMap) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

int per_element_offset(const SoeFieldInfo* info, int index) {
    if (info == nullptr) {
        return -1;
    }
    if (info->count > 1) {
        const int stride = info->stride != 0 ? info->stride : info->size;
        return info->offset + index * stride;
    }
    return info->offset;
}

int label_offset(std::string_view label) {
    const SoeFieldInfo* info = label_info(label);
    if (info == nullptr || info->count <= 1) {
        return info != nullptr ? info->offset : -1;
    }
    std::size_t index = 0;
    std::string_view sub;
    if (split_array_label(label, "weapon", index, sub) ||
        split_array_label(label, "child", index, sub) ||
        split_array_label(label, "path_point", index, sub)) {
        return per_element_offset(info, static_cast<int>(index));
    }
    return info->offset;
}

int guard_count(std::string_view what, const std::string& token, int max_value) {
    std::int64_t value = 0;
    if (!parse_int(token, value)) {
        fail("expected an integer, got \"" + token + "\" for " + std::string(what));
    }
    if (value < 0) {
        fail(std::string(what) + " is negative (" + token + ")");
    }
    if (value > max_value) {
        fail(std::string(what) + " is " + token + ", limit is " +
             std::to_string(max_value));
    }
    return static_cast<int>(value);
}

} // namespace

const char* soe_kind_name(SoeValueKind kind) {
    switch (kind) {
    case SoeValueKind::Int:
        return "int";
    case SoeValueKind::Double:
        return "double";
    case SoeValueKind::String:
        return "string";
    case SoeValueKind::Name16:
        return "name16";
    }
    return "?";
}

const char* soe_status_name(SoeStatus status) {
    switch (status) {
    case SoeStatus::Proven:
        return "proven";
    case SoeStatus::Hypothesis:
        return "hypothesis";
    case SoeStatus::Unknown:
        return "unknown";
    }
    return "?";
}

std::span<const SoeFieldInfo> field_map() { return kFieldMap; }

const SoeFieldInfo* label_info(std::string_view label) {
    for (const auto& info : kFieldMap) {
        if ((!info.legacy.empty() && info.legacy == label) ||
            (!info.soe.empty() && info.soe == label)) {
            return &info;
        }
    }

    std::size_t index = 0;
    std::string_view sub;
    if (split_array_label(label, "path_point", index, sub)) {
        return sub.empty() ? find_by_name("path_point") : nullptr;
    }
    if (split_array_label(label, "weapon", index, sub)) {
        if (sub == "name") {
            return find_by_name("weapon_id");
        }
        if (sub.size() == 2 && sub[0] == 'w' && sub[1] >= '0' && sub[1] <= '5') {
            const char name[] = {'w', 'e', 'a', 'p', 'o', 'n', '_', 'p', sub[1], '\0'};
            return find_by_name(name);
        }
        return nullptr;
    }
    if (split_array_label(label, "child", index, sub)) {
        if (sub == "name") {
            return find_by_name("child_type");
        }
        if (sub == "x") {
            return find_by_name("child_param");
        }
        if (sub.size() == 2 && sub[0] == 'c' && sub[1] >= '0' && sub[1] <= '5') {
            switch (sub[1]) {
            case '0':
                return find_by_name("child_x");
            case '1':
                return find_by_name("child_y");
            case '2':
                return find_by_name("child_z");
            case '3':
                return find_by_name("child_r1");
            case '4':
                return find_by_name("child_r2");
            case '5':
                return find_by_name("child_r3");
            default:
                break;
            }
        }
        return nullptr;
    }
    return nullptr;
}

const SoeField* ThingType::find(std::string_view label) const {
    for (const auto& field : fields) {
        if (field.label == label) {
            return &field;
        }
    }
    return nullptr;
}

std::int64_t ThingType::get_int(std::string_view label, std::int64_t fallback) const {
    const SoeField* field = find(label);
    return field != nullptr && field->kind == SoeValueKind::Int ? field->i : fallback;
}

double ThingType::get_double(std::string_view label, double fallback) const {
    const SoeField* field = find(label);
    return field != nullptr && field->kind == SoeValueKind::Double ? field->d : fallback;
}

std::string_view ThingType::get_string(std::string_view label,
                                       std::string_view fallback) const {
    const SoeField* field = find(label);
    return field != nullptr && field->kind == SoeValueKind::String ? field->s : fallback;
}

std::vector<std::uint8_t> SoeFile::to_bytes() const {
    return std::vector<std::uint8_t>(source.begin(), source.end());
}

std::optional<SoeFile> parse_soe(std::span<const std::uint8_t> data,
                                 std::string& error, std::string_view name) {
    error.clear();
    if (data.size() > kSoeMaxBytes) {
        error = ".soe is " + std::to_string(data.size()) + " bytes, limit is " +
                std::to_string(kSoeMaxBytes);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] == 0) {
            error = "NUL byte at offset " + std::to_string(i);
            return std::nullopt;
        }
    }

    try {
        const std::vector<std::string> tokens = tokenize(data);
        if (tokens.empty()) {
            error = ".soe is empty";
            return std::nullopt;
        }

        Reader reader(tokens);
        ThingType thing;
        thing.name = std::string(name);

        const auto emit_int = [&](std::string_view label) {
            SoeField field;
            field.label = std::string(label);
            field.kind = SoeValueKind::Int;
            field.i = reader.integer();
            field.offset = label_offset(label);
            thing.fields.push_back(std::move(field));
        };
        const auto emit_double = [&](std::string_view label) {
            SoeField field;
            field.label = std::string(label);
            field.kind = SoeValueKind::Double;
            field.d = reader.real();
            field.offset = label_offset(label);
            thing.fields.push_back(std::move(field));
        };
        const auto emit_string = [&](std::string_view label) {
            SoeField field;
            field.label = std::string(label);
            field.kind = SoeValueKind::String;
            field.s = reader.str();
            field.offset = label_offset(label);
            thing.fields.push_back(std::move(field));
        };
        const auto peek = [&]() -> const std::string& {
            if (reader.eof()) {
                fail("unexpected end of .soe");
            }
            return tokens[reader.position()];
        };

        thing.version = static_cast<int>(reader.integer());
        if (thing.version < 0 || thing.version > kSoeMaxVersion) {
            fail("version " + std::to_string(thing.version) + " is outside 0.." +
                 std::to_string(kSoeMaxVersion));
        }
        // The Python parser labels the version token too.
        {
            SoeField field;
            field.label = "version";
            field.kind = SoeValueKind::Int;
            field.i = thing.version;
            thing.fields.push_back(std::move(field));
        }
        const int version = thing.version;

        // id / class-name block
        emit_int("f19c");
        emit_int("f198");
        emit_string("class_name");
        thing.id = thing.fields.back().s;
        emit_int("f1a8");
        emit_int("f1ac");
        if (version >= 9) {
            emit_int("f1b0");
        }

        // movement token, six doubles, collision shape
        emit_string("collision_type");
        emit_double("col_f10");
        emit_double("col_f20");
        emit_double("col_f28");
        emit_double("col_f90");
        emit_double("col_f98");
        emit_double("col_fa0");
        std::string shape = reader.str();
        for (char& c : shape) {
            c = ascii_lower(c);
        }
        {
            SoeField field;
            field.label = "collision_shape";
            field.kind = SoeValueKind::String;
            field.s = shape;
            field.offset = label_offset("collision_shape");
            thing.fields.push_back(std::move(field));
        }
        if (shape == "ellipsoid") {
            emit_double("radius_x");
            emit_double("radius_y");
            emit_double("radius_z");
        } else if (shape == "sphere") {
            emit_double("radius");
        }

        emit_double("f50");
        emit_double("f58");
        emit_double("f60");
        emit_double("f68");

        if (version >= 7) {
            emit_string("event_a");
            emit_int("f1d4");
        } else {
            emit_string("event_a");
            emit_string("event_b");
            emit_int("f1d4");
        }

        if (version >= 8) {
            emit_int("shield0");
            emit_int("shield1");
            emit_int("shield2");
            emit_int("shield3");
        } else {
            emit_int("shield0");
        }

        emit_string("tex_a");
        emit_string("tex_b");

        emit_int("expire_value");
        if (version >= 3) {
            emit_string("expire_event");
        }

        emit_string("str_c8");
        emit_double("f78");
        emit_double("f80");
        emit_double("f88");
        emit_int("f1c8");
        emit_string("str_f0");
        emit_string("str_1a4");
        emit_int("f200");
        emit_int("f210");
        emit_int("f214");

        if (version >= 4) {
            emit_double("falling_fear");
        }
        if (version >= 5) {
            emit_string("create_event");
        }
        if (version >= 6) {
            emit_string("morale_failure_event");
            emit_string("time_out_event");
            emit_string("damaged_event");
        }

        if (version >= 7) {
            emit_int("f1cc");
            emit_int("f1d0");
        }
        if (version >= 10) {
            emit_double("f_a8");
            emit_double("f_b0");
        }
        if (version >= 11) {
            emit_double("f_b8");
            emit_double("f_c0");
        }
        if (version >= 12) {
            emit_double("f18");
            emit_double("f70");
        }
        if (version >= 13) {
            emit_string("leash_location");
            emit_double("f_d0");
        }

        if (version >= 2) {
            const int path_count = guard_count("path_point_count", peek(),
                                               kSoeMaxPathPoints);
            emit_int("path_point_count");
            for (int i = 0; i < path_count; ++i) {
                emit_string("path_point[" + std::to_string(i) + "]");
            }
        }

        const int weapon_count = guard_count("weapon_count", peek(), kSoeMaxWeapons);
        emit_int("weapon_count");
        for (int i = 0; i < weapon_count; ++i) {
            emit_string("weapon[" + std::to_string(i) + "].name");
            for (int k = 0; k < 6; ++k) {
                emit_int("weapon[" + std::to_string(i) + "].w" + std::to_string(k));
            }
        }

        if (version >= 1) {
            const int children_count = guard_count("children_count", peek(),
                                                   kSoeMaxChildren);
            emit_int("children_count");
            for (int i = 0; i < children_count; ++i) {
                emit_string("child[" + std::to_string(i) + "].name");
                for (int k = 0; k < 6; ++k) {
                    emit_double("child[" + std::to_string(i) + "].c" +
                                std::to_string(k));
                }
                emit_int("child[" + std::to_string(i) + "].x");
            }
        }

        thing.extra = reader.size() - reader.position();

        SoeFile file;
        file.name = thing.name;
        file.source.assign(reinterpret_cast<const char*>(data.data()), data.size());
        file.records.push_back(std::move(thing));
        return file;
    } catch (const ParseError& error_) {
        error = error_.what;
        return std::nullopt;
    }
}

} // namespace gm
