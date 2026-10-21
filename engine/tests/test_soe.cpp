// .soe / THING_TYPES reader: golden for the shipped corpus and grammar
// coverage for every version (0..13).
//
// The golden bytes are SOBS/railrock.soe (340 bytes, 67 tokens, version 13).
// kRailrockFields is the field list produced by gm/soe.py on those bytes; the
// per-version fixtures and their FNV-1a64 checksums were generated with the
// same parser (each one parses with extra == 0).  docs/SOE_CPP.md documents the
// API and the deliberate strictness differences from the Python reader.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "res/soe.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

const char* kind_code(gm::SoeValueKind kind) {
    switch (kind) {
    case gm::SoeValueKind::Int:
        return "i";
    case gm::SoeValueKind::Double:
        return "d";
    case gm::SoeValueKind::String:
        return "s";
    case gm::SoeValueKind::Name16:
        return "n";
    }
    return "?";
}

std::uint64_t fnv1a(std::uint64_t h, std::string_view bytes) {
    for (const char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string canonical(const gm::ThingType& thing) {
    std::string out;
    char number[64];
    for (std::size_t i = 0; i < thing.fields.size(); ++i) {
        const gm::SoeField& field = thing.fields[i];
        if (i != 0) {
            out.push_back('\0');
        }
        out += field.label;
        out.push_back('\0');
        out += kind_code(field.kind);
        out.push_back('\0');
        switch (field.kind) {
        case gm::SoeValueKind::Int:
            out += std::to_string(field.i);
            break;
        case gm::SoeValueKind::Double:
            std::snprintf(number, sizeof number, "%.17g", field.d);
            out += number;
            break;
        case gm::SoeValueKind::String:
            out += field.s;
            break;
        case gm::SoeValueKind::Name16:
            out += field.s;
            break;
        }
    }
    return out;
}

std::optional<gm::SoeFile> parse(std::string_view text, std::string& error,
                                 std::string_view name = {}) {
    return gm::parse_soe(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
        error, name);
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

static const char kRailrock[] =
    " 13 \r\n"
    " 0 \r\n"
    " 10 \r\n"
    "railrock\r\n"
    " 37 \r\n"
    " 116982 \r\n"
    " 8 \r\n"
    "PUSHABLE\r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    "ELLIPSOID\r\n"
    " 115 \r\n"
    " 115 \r\n"
    " 80 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    "0\r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    "NONE\r\n"
    "NONE\r\n"
    " 0 \r\n"
    "0\r\n"
    "0\r\n"
    " 3 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    "0\r\n"
    "NONE\r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    "0\r\n"
    "0\r\n"
    "0\r\n"
    "0\r\n"
    " 200 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 0 \r\n"
    " 50 \r\n"
    "0\r\n"
    " 0 \r\n"
    "0\r\n"
    " 1\r\n"
    "push_crate 1 128 10 0 0 0\r\n"
    " 0\r\n"

    "";

static const char kRailrockFields[] = R"FIELDS(version	int	13
f19c	int	0
f198	int	10
class_name	str	railrock
f1a8	int	37
f1ac	int	116982
f1b0	int	8
collision_type	str	PUSHABLE
col_f10	flt	0
col_f20	flt	0
col_f28	flt	0
col_f90	flt	0
col_f98	flt	0
col_fa0	flt	0
collision_shape	str	ellipsoid
radius_x	flt	115
radius_y	flt	115
radius_z	flt	80
f50	flt	0
f58	flt	0
f60	flt	0
f68	flt	0
event_a	str	0
f1d4	int	0
shield0	int	0
shield1	int	0
shield2	int	0
shield3	int	0
tex_a	str	NONE
tex_b	str	NONE
expire_value	int	0
expire_event	str	0
str_c8	str	0
f78	flt	3
f80	flt	0
f88	flt	0
f1c8	int	0
str_f0	str	0
str_1a4	str	NONE
f200	int	0
f210	int	0
f214	int	0
falling_fear	flt	0
create_event	str	0
morale_failure_event	str	0
time_out_event	str	0
damaged_event	str	0
f1cc	int	200
f1d0	int	0
f_a8	flt	0
f_b0	flt	0
f_b8	flt	0
f_c0	flt	0
f18	flt	0
f70	flt	50
leash_location	str	0
f_d0	flt	0
path_point_count	int	0
weapon_count	int	1
weapon[0].name	str	push_crate
weapon[0].w0	int	1
weapon[0].w1	int	128
weapon[0].w2	int	10
weapon[0].w3	int	0
weapon[0].w4	int	0
weapon[0].w5	int	0
children_count	int	0
)FIELDS";


struct Fixture {
    const char* text;
    std::size_t fields;
    std::uint64_t hash;
};

// Generated from gm/soe.py; each fixture is parsed by Python with extra == 0.
// v0: 36 fields, shape none, path 0, weapons 0, children 0
static const char kFixtureV0[] = R"SOE(0 1 2 type0 3 4 MOVE 6 1.5 7 8 9 10 none 15 16 17 18 EVENT_A EVENT_B 19 24 TEX_A TEX_B 25 STR_C8 26 27 28 29 STR_F0 SND 30 31 32 0)SOE";
static const Fixture kGoldenV0 = {kFixtureV0, 36, 0xf09f429330524851ull};

// v1: 68 fields, shape sphere, path 0, weapons 2, children 2
static const char kFixtureV1[] = R"SOE(1 11 12 type1 13 14 MOVE 16 1.5 17 18 19 20 sphere 14 15 16 17 18 EVENT_A EVENT_B 29 24 TEX_A TEX_B 35 STR_C8 26 27 28 39 STR_F0 SND 30 31 32 2 WEAP0 100 101 102 103 104 105 WEAP1 101 102 103 104 105 106 2 CHILD0 50 51 52 53 54 55 200 CHILD1 51 52 53 54 55 56 201)SOE";
static const Fixture kGoldenV1 = {kFixtureV1, 68, 0x716fa64b5e56800dull};

// v2: 43 fields, shape ellipsoid, path 2, weapons 0, children 0
static const char kFixtureV2[] = R"SOE(2 21 22 type2 23 24 MOVE 26 1.5 27 28 29 30 ellipsoid 11 12 13 15 16 17 18 EVENT_A EVENT_B 39 24 TEX_A TEX_B 45 STR_C8 26 27 28 49 STR_F0 SND 30 31 32 2 PATH0 PATH1 0 0)SOE";
static const Fixture kGoldenV2 = {kFixtureV2, 43, 0xb105a5fba770ef32ull};

// v3: 39 fields, shape none, path 0, weapons 0, children 0
static const char kFixtureV3[] = R"SOE(3 31 32 type3 33 34 MOVE 36 1.5 37 38 39 40 none 15 16 17 18 EVENT_A EVENT_B 49 24 TEX_A TEX_B 55 EXPIRE_EVENT STR_C8 26 27 28 59 STR_F0 SND 30 31 32 0 0 0)SOE";
static const Fixture kGoldenV3 = {kFixtureV3, 39, 0x9b9d1b531173c259ull};

// v4: 41 fields, shape sphere, path 0, weapons 0, children 0
static const char kFixtureV4[] = R"SOE(4 41 42 type4 43 44 MOVE 46 1.5 47 48 49 50 sphere 14 15 16 17 18 EVENT_A EVENT_B 59 24 TEX_A TEX_B 65 EXPIRE_EVENT STR_C8 26 27 28 69 STR_F0 SND 30 31 32 33 0 0 0)SOE";
static const Fixture kGoldenV4 = {kFixtureV4, 41, 0x33847333dc5690fbull};

// v5: 44 fields, shape ellipsoid, path 0, weapons 0, children 0
static const char kFixtureV5[] = R"SOE(5 51 52 type5 53 54 MOVE 56 1.5 57 58 59 60 ellipsoid 11 12 13 15 16 17 18 EVENT_A EVENT_B 69 24 TEX_A TEX_B 75 EXPIRE_EVENT STR_C8 26 27 28 79 STR_F0 SND 30 31 32 33 CREATE_EVENT 0 0 0)SOE";
static const Fixture kGoldenV5 = {kFixtureV5, 44, 0x62943513341a0dfeull};

// v6: 44 fields, shape none, path 0, weapons 0, children 0
static const char kFixtureV6[] = R"SOE(6 61 62 type6 63 64 MOVE 66 1.5 67 68 69 70 none 15 16 17 18 EVENT_A EVENT_B 79 24 TEX_A TEX_B 85 EXPIRE_EVENT STR_C8 26 27 28 89 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 0 0 0)SOE";
static const Fixture kGoldenV6 = {kFixtureV6, 44, 0xbb4d25af4d84ef3aull};

// v7: 47 fields, shape sphere, path 1, weapons 0, children 0
static const char kFixtureV7[] = R"SOE(7 71 72 type7 73 74 MOVE 76 1.5 77 78 79 80 sphere 14 15 16 17 18 EVENT_A 89 24 TEX_A TEX_B 95 EXPIRE_EVENT STR_C8 26 27 28 99 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 104 105 1 PATH0 0 0)SOE";
static const Fixture kGoldenV7 = {kFixtureV7, 47, 0x08128f320dad20f3ull};

// v8: 58 fields, shape ellipsoid, path 0, weapons 1, children 0
static const char kFixtureV8[] = R"SOE(8 81 82 type8 83 84 MOVE 86 1.5 87 88 89 90 ellipsoid 11 12 13 15 16 17 18 EVENT_A 99 20 21 22 23 TEX_A TEX_B 105 EXPIRE_EVENT STR_C8 26 27 28 109 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 114 115 0 1 WEAP0 100 101 102 103 104 105 0)SOE";
static const Fixture kGoldenV8 = {kFixtureV8, 58, 0x6d9ab788893ebfd8ull};

// v9: 57 fields, shape none, path 0, weapons 0, children 1
static const char kFixtureV9[] = R"SOE(9 91 92 type9 93 94 95 MOVE 96 1.5 97 98 99 100 none 15 16 17 18 EVENT_A 109 20 21 22 23 TEX_A TEX_B 115 EXPIRE_EVENT STR_C8 26 27 28 119 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 124 125 0 0 1 CHILD0 50 51 52 53 54 55 200)SOE";
static const Fixture kGoldenV9 = {kFixtureV9, 57, 0x41f5049be5fe1d30ull};

// v10: 52 fields, shape sphere, path 0, weapons 0, children 0
static const char kFixtureV10[] = R"SOE(10 101 102 type10 103 104 105 MOVE 106 1.5 107 108 109 110 sphere 14 15 16 17 18 EVENT_A 119 20 21 22 23 TEX_A TEX_B 125 EXPIRE_EVENT STR_C8 26 27 28 129 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 134 135 36 37 0 0 0)SOE";
static const Fixture kGoldenV10 = {kFixtureV10, 52, 0xf7b67c646e1d7156ull};

// v11: 56 fields, shape ellipsoid, path 0, weapons 0, children 0
static const char kFixtureV11[] = R"SOE(11 111 112 type11 113 114 115 MOVE 116 1.5 117 118 119 120 ellipsoid 11 12 13 15 16 17 18 EVENT_A 129 20 21 22 23 TEX_A TEX_B 135 EXPIRE_EVENT STR_C8 26 27 28 139 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 144 145 36 37 38 39 0 0 0)SOE";
static const Fixture kGoldenV11 = {kFixtureV11, 56, 0x327e88ae7eb07653ull};

// v12: 55 fields, shape none, path 0, weapons 0, children 0
static const char kFixtureV12[] = R"SOE(12 121 122 type12 123 124 125 MOVE 126 1.5 127 128 129 130 none 15 16 17 18 EVENT_A 139 20 21 22 23 TEX_A TEX_B 145 EXPIRE_EVENT STR_C8 26 27 28 149 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 154 155 36 37 38 39 40 41 0 0 0)SOE";
static const Fixture kGoldenV12 = {kFixtureV12, 55, 0x27284e960bba21d9ull};

// v13: 90 fields, shape sphere, path 2, weapons 2, children 2
static const char kFixtureV13[] = R"SOE(13 131 132 type13 133 134 135 MOVE 136 1.5 137 138 139 140 sphere 14 15 16 17 18 EVENT_A 149 20 21 22 23 TEX_A TEX_B 155 EXPIRE_EVENT STR_C8 26 27 28 159 STR_F0 SND 30 31 32 33 CREATE_EVENT MORALE TIMEOUT DAMAGED 164 165 36 37 38 39 40 41 LEASH 42 2 PATH0 PATH1 2 WEAP0 100 101 102 103 104 105 WEAP1 101 102 103 104 105 106 2 CHILD0 50 51 52 53 54 55 200 CHILD1 51 52 53 54 55 56 201)SOE";
static const Fixture kGoldenV13 = {kFixtureV13, 90, 0x9dd5a3f2a5e5135cull};

static const char kMinimalV0[] = R"SOE(0 1 2 type0 3 4 MOVE 6 1.5 7 8 9 10 none 15 16 17 18 EVENT_A EVENT_B 19 24 TEX_A TEX_B 25 STR_C8 26 27 28 29 STR_F0 SND 30 31 32 0)SOE";
static const char kMinimalV1[] = R"SOE(1 11 12 type1 13 14 MOVE 16 1.5 17 18 19 20 none 15 16 17 18 EVENT_A EVENT_B 29 24 TEX_A TEX_B 35 STR_C8 26 27 28 39 STR_F0 SND 30 31 32 0 0)SOE";
static const char kMinimalV2[] = R"SOE(2 21 22 type2 23 24 MOVE 26 1.5 27 28 29 30 none 15 16 17 18 EVENT_A EVENT_B 39 24 TEX_A TEX_B 45 STR_C8 26 27 28 49 STR_F0 SND 30 31 32 0 0 0)SOE";
static const char kOverWeapons[] = R"SOE(0 1 2 type0 3 4 MOVE 6 1.5 7 8 9 10 none 15 16 17 18 EVENT_A EVENT_B 19 24 TEX_A TEX_B 25 STR_C8 26 27 28 29 STR_F0 SND 30 31 32 9 WEAP0 100 101 102 103 104 105 WEAP1 101 102 103 104 105 106 WEAP2 102 103 104 105 106 107 WEAP3 103 104 105 106 107 108 WEAP4 104 105 106 107 108 109 WEAP5 105 106 107 108 109 110 WEAP6 106 107 108 109 110 111 WEAP7 107 108 109 110 111 112 WEAP8 108 109 110 111 112 113)SOE";
static const char kOverChildren[] = R"SOE(1 11 12 type1 13 14 MOVE 16 1.5 17 18 19 20 none 15 16 17 18 EVENT_A EVENT_B 29 24 TEX_A TEX_B 35 STR_C8 26 27 28 39 STR_F0 SND 30 31 32 0 5 CHILD0 50 51 52 53 54 55 200 CHILD1 51 52 53 54 55 56 201 CHILD2 52 53 54 55 56 57 202 CHILD3 53 54 55 56 57 58 203 CHILD4 54 55 56 57 58 59 204)SOE";
static const char kOverPath[] = R"SOE(2 21 22 type2 23 24 MOVE 26 1.5 27 28 29 30 none 15 16 17 18 EVENT_A EVENT_B 39 24 TEX_A TEX_B 45 STR_C8 26 27 28 49 STR_F0 SND 30 31 32 33 PATH0 PATH1 PATH2 PATH3 PATH4 PATH5 PATH6 PATH7 PATH8 PATH9 PATH10 PATH11 PATH12 PATH13 PATH14 PATH15 PATH16 PATH17 PATH18 PATH19 PATH20 PATH21 PATH22 PATH23 PATH24 PATH25 PATH26 PATH27 PATH28 PATH29 PATH30 PATH31 PATH32 0 0)SOE";


std::string canonical_map() {
    std::string out;
    bool first = true;
    const auto add = [&](std::string_view part) {
        if (!first) {
            out.push_back('\0');
        }
        first = false;
        out += part;
    };
    for (const auto& info : gm::field_map()) {
        add(std::to_string(info.offset));
        add(std::to_string(info.size));
        add(gm::soe_kind_name(info.kind));
        add(info.name);
        add(info.role);
        add(gm::soe_status_name(info.status));
        add(info.soe);
        add(info.legacy);
        add(std::to_string(info.count));
        add(std::to_string(info.stride));
        add(std::to_string(info.instance));
        add(info.loader);
        add(info.xref);
    }
    return out;
}

void test_field_map() {
    const auto map = gm::field_map();
    check(map.size() == 77, "field_map has 77 rows (found " +
                                std::to_string(map.size()) + ")");
    std::size_t proven = 0;
    std::size_t hypothesis = 0;
    std::size_t unknown = 0;
    for (const auto& info : map) {
        switch (info.status) {
        case gm::SoeStatus::Proven:
            ++proven;
            break;
        case gm::SoeStatus::Hypothesis:
            ++hypothesis;
            break;
        case gm::SoeStatus::Unknown:
            ++unknown;
            break;
        }
        check(!info.name.empty(), "field map row has a name");
        check(!info.role.empty(), std::string(info.name) + " has a role");
        check(info.size == 4 || info.size == 8 || info.size == 16,
              std::string(info.name) + " has a valid size");
        if (info.kind == gm::SoeValueKind::Int) {
            check(info.size == 4, std::string(info.name) + " int is 4 bytes");
        } else if (info.kind == gm::SoeValueKind::Double) {
            check(info.size == 8, std::string(info.name) + " double is 8 bytes");
        } else {
            check(info.size == 16, std::string(info.name) + " name16 is 16 bytes");
        }
        if (info.count > 1) {
            check(info.stride > 0, std::string(info.name) + " array has a stride");
        }
    }
    check(proven == 41, "41 proven rows (found " + std::to_string(proven) + ")");
    check(hypothesis == 21,
          "21 hypothesis rows (found " + std::to_string(hypothesis) + ")");
    check(unknown == 15, "15 unknown rows (found " + std::to_string(unknown) + ")");
    check(gm::kSoeRecordSize == 936, "THING_TYPES record is 936 bytes");
    // Generated from gm/soe.py FIELD_MAP: every string, offset, status and
    // xref must stay identical to the Python table.
    check(fnv1a(0xcbf29ce484222325ull, canonical_map()) == 0x1d4287f700b7c8a4ull,
          "field map matches the gm/soe.py golden hash");

    const gm::SoeFieldInfo* sob = gm::label_info("class_name");
    check(sob != nullptr, "class_name has a field-map row");
    if (sob != nullptr) {
        check(sob->offset == 0x1A4, "class_name lands at +0x1a4");
        check(sob->name == "sob_index", "class_name row is sob_index");
        check(sob->status == gm::SoeStatus::Proven, "sob_index is proven");
        check(sob->kind == gm::SoeValueKind::Int, "sob_index is an int");
    }

    // Every scalar .soe label and every array pattern resolves back to a row.
    for (const auto& info : map) {
        if (info.soe.empty() || info.soe.find('/') != std::string_view::npos) {
            continue;
        }
        std::string label(info.soe);
        const std::size_t pattern = label.find("[i]");
        if (pattern != std::string::npos) {
            label.replace(pattern, 3, "[0]");
        }
        const gm::SoeFieldInfo* back = gm::label_info(label);
        check(back != nullptr, "label_info(" + label + ") resolves");
        if (back != nullptr) {
            check(back->offset == info.offset,
                  "label_info(" + label + ") keeps the offset");
        }
    }
    check(gm::label_info("version") == nullptr, "version has no record offset");
    check(gm::label_info("nonsense") == nullptr, "unknown label has no row");
    check(gm::label_info("weapon[3].w5") != nullptr, "weapon array label resolves");
    check(gm::label_info("weapon[3].w6") == nullptr, "weapon w6 does not resolve");
    check(gm::label_info("child[2].c5") != nullptr, "child array label resolves");
    check(gm::label_info("path_point[31]") != nullptr, "path point label resolves");
    const gm::SoeFieldInfo* p2 = gm::label_info("weapon[1].w2");
    check(p2 != nullptr && p2->name == "weapon_p2", "weapon w2 maps to weapon_p2");
}

void test_railrock_golden() {
    const std::string_view bytes(kRailrock, sizeof kRailrock - 1);
    check(bytes.size() == 340, "railrock golden is 340 bytes (found " +
                                   std::to_string(bytes.size()) + ")");

    std::string error;
    auto file = parse(bytes, error, "railrock");
    if (!file) {
        check(false, "railrock parses: " + error);
        return;
    }
    const gm::ThingType& thing = file->thing();
    check(file->name == "railrock", "thing name comes from the caller");
    check(thing.name == "railrock", "record name is the thing-type name");
    check(thing.id == "railrock", "class_name token is the model id");
    check(thing.version == 13, "railrock version 13");
    check(thing.fields.size() == 67,
          "railrock has 67 fields (found " + std::to_string(thing.fields.size()) +
              ")");
    check(thing.extra == 0, "railrock consumes every token");
    check(thing.complete(), "railrock is complete");
    check(file->records.size() == 1, "one record per .soe");
    check(file->source.size() == bytes.size(), "source is kept verbatim");
    check(file->to_bytes() ==
              std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
          "to_bytes is byte-exact");

    // Every field compared against the Python parser's output.
    std::vector<std::string> expected;
    std::string_view block(kRailrockFields);
    while (!block.empty()) {
        const std::size_t end = block.find('\n');
        expected.emplace_back(block.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        block.remove_prefix(end + 1);
    }
    check(expected.size() == thing.fields.size(),
          "golden field count matches Python (found " +
              std::to_string(expected.size()) + ")");
    for (std::size_t i = 0; i < expected.size() && i < thing.fields.size(); ++i) {
        const std::string& row = expected[i];
        const std::size_t t1 = row.find('\t');
        const std::size_t t2 = row.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) {
            check(false, "malformed golden row " + std::to_string(i));
            continue;
        }
        const std::string label = row.substr(0, t1);
        const std::string kind = row.substr(t1 + 1, t2 - t1 - 1);
        const std::string value = row.substr(t2 + 1);
        const gm::SoeField& field = thing.fields[i];
        check(field.label == label, "field " + std::to_string(i) + " label " +
                                        field.label + " == " + label);
        const bool kind_ok =
            (kind == "int" && field.kind == gm::SoeValueKind::Int) ||
            (kind == "flt" && field.kind == gm::SoeValueKind::Double) ||
            (kind == "str" && field.kind == gm::SoeValueKind::String);
        check(kind_ok, "field " + label + " kind");
        if (field.kind == gm::SoeValueKind::Int) {
            check(field.i == std::strtoll(value.c_str(), nullptr, 10),
                  "field " + label + " int value");
        } else if (field.kind == gm::SoeValueKind::Double) {
            check(field.d == std::strtod(value.c_str(), nullptr),
                  "field " + label + " double value");
        } else {
            check(field.s == value, "field " + label + " string value");
        }
        if (label != "version") {
            const gm::SoeFieldInfo* info = gm::label_info(label);
            check(info != nullptr, "field " + label + " has a map row");
            check(field.offset >= 0, "field " + label + " has an offset");
            if (info != nullptr) {
                check(field.offset == info->offset ||
                          (info->count > 1 && field.offset >= info->offset),
                      "field " + label + " offset matches the map");
            }
        }
    }

    check(thing.get_int("f19c") == 0, "f19c == 0");
    check(thing.get_int("f198") == 10, "f198 == 10");
    check(thing.get_int("f1a8") == 37, "f1a8 == 37");
    check(thing.get_int("f1ac") == 116982, "f1ac == 116982");
    check(thing.get_int("f1b0") == 8, "f1b0 == 8");
    check(thing.get_string("collision_type") == "PUSHABLE", "movement PUSHABLE");
    check(thing.get_string("collision_shape") == "ellipsoid", "shape ellipsoid");
    check(thing.get_double("radius_x") == 115.0, "radius_x 115");
    check(thing.get_double("radius_y") == 115.0, "radius_y 115");
    check(thing.get_double("radius_z") == 80.0, "radius_z 80");
    check(thing.get_double("f78") == 3.0, "f78 3");
    check(thing.get_double("f70") == 50.0, "f70 50");
    check(thing.get_int("f1cc") == 200, "f1cc 200");
    check(thing.get_int("weapon_count") == 1, "one weapon");
    check(thing.get_string("weapon[0].name") == "push_crate", "weapon push_crate");
    check(thing.get_int("weapon[0].w1") == 128, "weapon w1 128");
    check(thing.get_int("weapon[0].w2") == 10, "weapon w2 10");
    check(thing.get_int("children_count") == 0, "no children");
    check(thing.get_int("path_point_count") == 0, "no path points");
    check(thing.get_int("f1d4") == 0, "health token 0");
    check(thing.get_int("shield0") == 0 && thing.get_int("shield3") == 0,
          "four shield tokens");
    check(thing.get_string("leash_location") == "0", "leash location");
    check(thing.get_double("f_d0") == 0.0, "leash distance");

    const gm::SoeField* class_name = thing.find("class_name");
    check(class_name != nullptr && class_name->offset == 0x1A4,
          "parsed class_name offset +0x1a4");
    const gm::SoeField* w2 = thing.find("weapon[0].w2");
    check(w2 != nullptr && w2->offset == 0x27C, "weapon[0].w2 offset +0x27c");
    check(thing.get_int("missing", -7) == -7, "missing int fallback");
    check(thing.get_double("missing", -1.5) == -1.5, "missing double fallback");
    check(thing.get_string("missing", "?") == "?", "missing string fallback");
}

void test_version_fixtures() {
    const Fixture fixtures[] = {
        kGoldenV0,  kGoldenV1,  kGoldenV2,  kGoldenV3,  kGoldenV4,
        kGoldenV5,  kGoldenV6,  kGoldenV7,  kGoldenV8,  kGoldenV9,
        kGoldenV10, kGoldenV11, kGoldenV12, kGoldenV13,
    };
    for (std::size_t i = 0; i < std::size(fixtures); ++i) {
        std::string error;
        auto file = parse(fixtures[i].text, error, "v" + std::to_string(i));
        if (!file) {
            check(false, "fixture v" + std::to_string(i) + " parses: " + error);
            continue;
        }
        const gm::ThingType& thing = file->thing();
        check(thing.version == static_cast<int>(i),
              "fixture v" + std::to_string(i) + " version");
        check(thing.fields.size() == fixtures[i].fields,
              "fixture v" + std::to_string(i) + " has " +
                  std::to_string(fixtures[i].fields) + " fields (found " +
                  std::to_string(thing.fields.size()) + ")");
        check(thing.extra == 0, "fixture v" + std::to_string(i) + " consumes all");
        check(fnv1a(0xcbf29ce484222325ull, canonical(thing)) == fixtures[i].hash,
              "fixture v" + std::to_string(i) + " value hash");
        check(file->to_bytes() ==
                  std::vector<std::uint8_t>(fixtures[i].text,
                                            fixtures[i].text +
                                                std::strlen(fixtures[i].text)),
              "fixture v" + std::to_string(i) + " round-trip");

        // Version gates.
        check((thing.find("f1b0") != nullptr) == (i >= 9), "f1b0 gate v>=9");
        check((thing.find("event_b") != nullptr) == (i < 7), "event_b gate v<7");
        check((thing.find("shield1") != nullptr) == (i >= 8), "shield1 gate v>=8");
        check((thing.find("shield3") != nullptr) == (i >= 8), "shield3 gate v>=8");
        check(thing.find("expire_event") != nullptr == (i >= 3),
              "expire_event gate v>=3");
        check(thing.find("falling_fear") != nullptr == (i >= 4),
              "falling_fear gate v>=4");
        check(thing.find("create_event") != nullptr == (i >= 5),
              "create_event gate v>=5");
        check(thing.find("morale_failure_event") != nullptr == (i >= 6),
              "morale gate v>=6");
        check((thing.find("f1cc") != nullptr) == (i >= 7), "f1cc gate v>=7");
        check((thing.find("f_a8") != nullptr) == (i >= 10), "f_a8 gate v>=10");
        check((thing.find("f_b8") != nullptr) == (i >= 11), "f_b8 gate v>=11");
        check((thing.find("f18") != nullptr) == (i >= 12), "f18 gate v>=12");
        check(thing.find("leash_location") != nullptr == (i >= 13),
              "leash gate v>=13");
        check(thing.find("path_point_count") != nullptr == (i >= 2),
              "path gate v>=2");
        check(thing.find("children_count") != nullptr == (i >= 1),
              "children gate v>=1");
        const std::size_t shape = i % 3;
        check((thing.find("radius") != nullptr) == (shape == 1), "sphere radius");
        check((thing.find("radius_x") != nullptr) == (shape == 2),
              "ellipsoid radii");
        check(thing.id == "type" + std::to_string(i), "fixture id token");
        if (i == 13) {
            const gm::SoeField* w = thing.find("weapon[1].w0");
            const gm::SoeField* c = thing.find("child[1].c0");
            const gm::SoeField* p = thing.find("path_point[1]");
            check(w != nullptr && w->offset == 0x240, "weapon[1].w0 offset +0x240");
            check(c != nullptr && c->offset == 0x0F0, "child[1].c0 offset +0x0f0");
            check(p != nullptr && p->offset == 0x328, "path_point[1] offset +0x328");
        }
    }
}

void test_negative() {
    std::string error;
    check(!parse("", error), "empty .soe is rejected");
    check(!error.empty(), "empty .soe sets an error");
    check(!parse(" \r\n\t ", error), "whitespace-only .soe is rejected");
    check(error.find("empty") != std::string::npos, "empty error message");

    const std::string_view bytes(kRailrock, sizeof kRailrock - 1);
    // Token-boundary truncations: dropping any of the 67 tokens must fail.
    // (Dropping only trailing CRLF leaves the token stream intact and parses.)
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < bytes.size();) {
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\r' ||
                                    bytes[i] == '\n' || bytes[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < bytes.size() && bytes[i] != ' ' && bytes[i] != '\r' &&
               bytes[i] != '\n' && bytes[i] != '\t') {
            ++i;
        }
        if (i > start) {
            tokens.emplace_back(bytes.substr(start, i - start));
        }
    }
    check(tokens.size() == 67, "golden tokenises to 67 tokens");
    std::size_t truncated_ok = 0;
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        std::string prefix;
        for (std::size_t i = 0; i < k; ++i) {
            if (i != 0) {
                prefix.push_back(' ');
            }
            prefix += tokens[i];
        }
        error.clear();
        if (parse(prefix, error, "railrock")) {
            ++truncated_ok;
        } else if (error.empty()) {
            check(false, "truncation at token " + std::to_string(k) +
                             " sets an error");
        }
    }
    check(truncated_ok == 0,
          "every token truncation fails (parsed " + std::to_string(truncated_ok) + ")");

    const std::string huge(4097, 'A');
    check(!parse(huge, error), "4097-byte token is rejected");
    check(error.find("limit") != std::string::npos, "huge token error mentions limit");

    std::string nul = std::string(kMinimalV0);
    nul.insert(nul.size() / 2, 1, '\0');
    check(!parse(nul, error), "NUL byte is rejected");
    check(error.find("NUL") != std::string::npos, "NUL error message");

    check(!parse(std::string_view("\0 1 2", 5), error), "leading NUL is rejected");

    const std::string too_big(gm::kSoeMaxBytes + 1, ' ');
    check(!parse(too_big, error), "oversized .soe is rejected");
    check(error.find("limit") != std::string::npos, "size error mentions limit");

    std::string many_tokens;
    for (std::size_t i = 0; i <= gm::kSoeMaxTokens; ++i) {
        many_tokens += "0 ";
    }
    check(!parse(many_tokens, error), "token-count limit is enforced");

    check(!parse(kOverWeapons, error), "9 weapons are rejected");
    check(error.find("weapon_count") != std::string::npos,
          "weapon limit error names the count");
    check(!parse(kOverChildren, error), "5 children are rejected");
    check(error.find("children_count") != std::string::npos,
          "children limit error names the count");
    check(!parse(kOverPath, error), "33 path points are rejected");
    check(error.find("path_point_count") != std::string::npos,
          "path limit error names the count");

    check(!parse("14 0 0 x 0 0", error), "version 14 is rejected");
    check(error.find("version") != std::string::npos, "version error message");
    check(!parse("-1", error), "negative version is rejected");

    std::string negative = kMinimalV0;
    const std::size_t last = negative.rfind(' ');
    negative = negative.substr(0, last) + " -1";
    check(!parse(negative, error), "negative weapon count is rejected");

    check(!parse("x", error), "non-numeric version is rejected");
    check(!parse("0 0 notanint name 1 2 MOVE 0 0 0 0 0 0 none", error),
          "non-numeric integer token is rejected");
    check(!parse("0 1 2 name 1 2 MOVE 0 0 0 0 0 0 sphere nope", error),
          "non-numeric double token is rejected");

    // Trailing tokens are accepted (ThingType.extra), like the Python parser.
    std::string trailing = std::string(kMinimalV0) + " EXTRA 1";
    auto file = parse(trailing, error, "extra");
    check(file.has_value(), "trailing tokens parse: " + error);
    if (file) {
        check(file->thing().extra == 2, "extra counts trailing tokens");
        check(!file->thing().complete(), "trailing tokens are incomplete");
        check(file->to_bytes() == std::vector<std::uint8_t>(trailing.begin(),
                                                           trailing.end()),
              "trailing tokens round-trip");
    }
}

void test_shipped_corpus() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path dir = game / "SOBS";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cout << "soe: game not installed, shipped corpus skipped\n";
        return;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (lower(entry.path().extension().string()) == ".soe") {
            files.push_back(entry.path());
        }
    }
    check(files.size() == 1,
          "1 shipped .soe file (found " + std::to_string(files.size()) + ")");
    for (const auto& path : files) {
        const std::vector<std::uint8_t> bytes = read_file(path);
        std::string error;
        auto file = gm::parse_soe(bytes, error, path.stem().string());
        if (!file) {
            check(false, path.filename().string() + " parses: " + error);
            continue;
        }
        check(file->thing().complete(),
              path.filename().string() + " consumes every token");
        check(file->to_bytes() == bytes,
              path.filename().string() + " round-trips byte-exactly");
        check(file->source.size() == 340,
              path.filename().string() + " is 340 bytes");
        check(file->thing().fields.size() == 67,
              path.filename().string() + " has 67 fields");
        if (file->name == "railrock") {
            check(std::vector<std::uint8_t>(kRailrock,
                                            kRailrock + sizeof kRailrock - 1) ==
                      bytes,
                  "installed railrock.soe equals the embedded golden");
        }
    }
}

} // namespace

int main() {
    test_field_map();
    test_railrock_golden();
    test_version_fixtures();
    test_negative();
    test_shipped_corpus();

    if (failures != 0) {
        std::cerr << "soe: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "soe: all checks passed\n";
    return 0;
}
