#include "res/sbtvm.h"

#include "core/bytes.h"

#include <bit>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <utility>

namespace gm {
namespace {

// ---------------------------------------------------------------------------
// Command tables (data recovered from gundx.exe at 0x4b53e8 / 0x4b62ec).
// One letter per operand: A/B/D = 8-byte double, Y = 1 byte, S = C string,
// every other letter = 4 bytes.  The ids absent from both tables are the
// structural opcodes handled explicitly below.
// ---------------------------------------------------------------------------

struct CommandDef {
    std::uint8_t id;
    const char* name;
    const char* sig; // nullptr: no operands
};

constexpr CommandDef kCommands[] = {
    {1, "EXIT_EVENT", nullptr},
    {2, "GENERATE", "E"},
    {3, "GENERATE_FOR_CHILDREN", "E"},
    {4, "GENERATE_FOR_PARENT", "E"},
    {5, "ENABLE", "E"},
    {6, "DISABLE", "E"},
    {7, "SLEEP", "P"},
    {8, "SET_SECTOR_MOVEMENT", "LLLLAAA"},
    {9, "SET_SECTOR_BRIGHTNESS", "LL"},
    {10, "SET_SECTOR_TYPE", "LL"},
    {11, "SET_SECTOR_DAMAGE", "LL"},
    {12, "SET_SECTOR_SOUND", "LVND"},
    {13, "SET_SECTOR_EVENTS", "LEE"},
    {14, "SET_SECTOR_FLAGS", "LL"},
    {15, "SET_POLY_TEXTURE", "LLLT"},
    {16, "SET_POLY_BRIGHTNESS", "LLLL"},
    {17, "SET_POLY_SCROLL", "LLLLLLL"},
    {18, "LOG_MESSAGE", "S"},
    {19, "EXIT_PROGRAM", nullptr},
    {20, "PLAY_SAMPLE", "NVLLL"},
    {21, "PLAY_SAMPLE_AT_LOCATION", "NVLLLLL"},
    {22, "SPEAK_NUMBER", "L"},
    {23, "SPEAK_SAMPLE", "NVLL"},
    {24, "SET_SOURCEWORLDOBJECT_HIT_OFF", nullptr},
    {25, "SET_SOURCEWORLDOBJECT_HIT_ON", nullptr},
    {26, "SET_SOURCEWORLDOBJECT_TEXTURES", "T"},
    {27, "SET_SOURCESECTOR_MOVEMENT", "LLLAAA"},
    {28, "SET_SOURCESECTOR_BRIGHTNESS", "L"},
    {29, "SET_SOURCESECTOR_FLAGS", "L"},
    {30, "SET_SOURCESECTOR_TYPE", "L"},
    {31, "SET_SOURCESECTOR_DAMAGE", "L"},
    {32, "SET_SOURCEPOLY_TEXTURE", "T"},
    {33, "SET_SOURCEPOLY_BRIGHTNESS", "L"},
    {34, "SET_SOURCEPOLY_MATERIAL", "M"},
    {35, "SET_SOURCEPOLY_SCROLL", "LLLL"},
    {36, "SET_SOURCEPOLY_SWITCH_EVENT", "E"},
    {37, "PLAY_SAMPLE_AT_SOURCE", "NVLL"},
    {38, "PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE", "LENVL"},
    {39, "REMOVE_SOURCE_ITEM", nullptr},
    {40, "SET_HANDLESECTOR_MOVEMENT", "ELLLAAA"},
    {41, "SET_HANDLESECTOR_BRIGHTNESS", "EL"},
    {42, "SET_HANDLESECTOR_TYPE", "EL"},
    {43, "SET_HANDLESECTOR_DAMAGE", "EL"},
    {44, "SET_HANDLESECTOR_FLAGS", "EL"},
    {45, "SET_HANDLESECTOR_SOUND", "ENVD"},
    {46, "SET_HANDLEPOLY_TEXTURE", "LT"},
    {47, "SET_HANDLEPOLY_BRIGHTNESS", "LL"},
    {48, "SET_HANDLEPOLY_SCROLL", "LLLLL"},
    {49, "SET_HANDLEPOLY_MATERIAL", "LM"},
    {50, "ADD_SCREEN_TEXT", "LS"},
    {51, "ADD_SOURCESCREEN_TEXT", "LS"},
    {52, "ADD_ITEM_AT_SOURCE", "OPPEELP"},
    {53, "ADD_ITEM", "OPPEELLLLAAAP"},
    {54, "ADD_ITEM_AT_SOURCE_FIXED", "OPPEELP"},
    {55, "ADD_ITEM_FIXED", "OPPEELLLLAAAP"},
    {56, "SET_DOOR_DELTAS", "LLLLAAA"},
    {57, "SET_DOOR_POSITION", "LL"},
    {58, "SET_DOOR_SOUNDS", "LNVNVNVD"},
    {59, "SET_DOOR_SPEED", "LL"},
    {60, "NEW_GAME", "S"},
    {61, "NEW_LEVEL", "S"},
    {62, "NEXT_LEVEL", nullptr},
    {63, "END_GAME", nullptr},
    {64, "ITEMS_MOVE_CHECK", nullptr},
    {65, "SET_SOURCEITEM_TRIGGER_EVENT", "E"},
    {66, "ADD_SPRITE", "RDDDLLLLMDDL"},
    {67, "ADD_SPRITE_AT_SOURCE", "RLLLLLLLMDDL"},
    {68, "ADD_SPRITE_AT_SOURCE_WEAPON_POINT", "LRLMDDL"},
    {69, "ADD_SPRITE_MOVING", "RLLLLMDDLDDDP"},
    {70, "ADD_SPRITE_MOVING_AT_SOURCE", "RLLLLLLLMDDLDDDP"},
    {71, "ADD_SPRITE_MOVING_AT_SOURCEPLAYER", "RLLLLLLLMDDLDDDP"},
    {72, "ADD_SPRITE_MOVING_AT_SOURCE_WEAPON_POINT", "LRLMDDLDDDP"},
    {73, "ADD_SOURCEPLAYER_SPRITE", "LRLLLLLLLMDDLL"},
    {74, "REMOVE_SOURCEPLAYER_SPRITE", "L"},
    {75, "CAUSE_BLAST_RADIUS_DAMAGE", "LLLLLE"},
    {76, "SOURCE_CLEAR", nullptr},
    {77, "SET_CAMERA_PITCH_SHAKE", "APP"},
    {78, "SET_CAMERA_ROLL_SHAKE", "APP"},
    {79, "SET_CAMERA_YAW_SHAKE", "APP"},
    {80, "SET_CAMERA_HAZE", "DD"},
    {81, "LOAD_MUSIC", "LS"},
    {82, "PLAY_MUSIC", "L"},
    {83, "STOP_MUSIC", nullptr},
    {84, "UNLOAD_MUSIC", "L"},
    {85, "MEMORIZE_CURRENT_MUSIC_BANK", nullptr},
    {86, "PLAY_MEMORIZED_MUSIC_BANK", nullptr},
    {87, "ADD_LIGHT_SOURCE", "LLLLLLLLLLLLLLLL"},
    {88, "ADD_SKY_LIGHT_SOURCE", "LLLLLLL"},
    {89, "SET_SECTOR_LINK", "LL"},
    {90, "SET_LEVEL_NAME", "S"},
    {91, "SHOW_LEVEL_END_SCREEN", nullptr},
    {92, "SET_LEVEL_END_CAMERA_XYZ", "LLL"},
    {93, "SET_LEVEL_BEST_TIME", "L"},
    {94, "SET_LEVEL_SECRETS", "L"},
    {95, "INCREMENT_SOURCEPLAYER_SECRET_TALLY", nullptr},
    {96, "SET_LEVEL_GRAVITY", "D"},
    {97, "SET_LEVEL_FOG_COLOUR", "LLL"},
    {98, "SET_LEVEL_FOG_DISTANCE", "L"},
    {99, "SET_LEVEL_COUNTDOWN_TIMER", "P"},
    {100, "SET_LEVEL_FOCAL_ADJUST", "D"},
    {101, "SET_LEVEL_ALIASING_VALUES", "DD"},
    {102, "SET_LEVEL_VERSION", "L"},
    {104, "OR_LEVEL_OBJECTIVES_BITS", "L"},
    {105, "AND_SOURCEPLAYER_KEY_BITS", "L"},
    {106, "OR_SOURCEPLAYER_KEY_BITS", "L"},
    {107, "AND_SOURCEPLAYER_WEAPON_BITS", "L"},
    {108, "OR_SOURCEPLAYER_WEAPON_BITS", "L"},
    {109, "ADD_SOURCEPLAYER_AMMO", "LL"},
    {110, "AND_SOURCEPLAYER_EQUIPMENT_BITS", "L"},
    {111, "OR_SOURCEPLAYER_EQUIPMENT_BITS", "L"},
    {112, "HEAL_SOURCEPLAYER", "LL"},
    {113, "REPAIR_SOURCEPLAYER", "LL"},
    {114, "ADD_SOURCEPLAYER_CASH", "L"},
    {115, "ADD_SOURCEPLAYER_SALVAGE", "L"},
    {116, "CHANGE_SOURCEPLAYER_SOB", "O"},
    {117, "SOURCEPLAYER_TURBO_BOOST", "P"},
    {118, "SOURCEPLAYER_RECOIL", "D"},
    {119, "SOURCEPLAYER_COLLISION_TYPE", "LLLL"},
    {120, "SOURCEPLAYER_HAZE", "P"},
    {121, "SOURCEPLAYER_HAZE_BRIGHTNESS", "L"},
    {122, "SOURCEPLAYER_INVINCIBLE", "P"},
    {123, "SOURCEPLAYER_INFECT", nullptr},
    {124, "SOURCEPLAYER_CURE", nullptr},
    {125, "SOURCEPLAYER_HIGHEST_WEAPON_WITH_AMMO", "L"},
    {126, "SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO", nullptr},
    {127, "SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO", nullptr},
    {128, "SOURCEPLAYER_FLY", "L"},
    {129, "SOURCEPLAYER_HEADLIGHT", "L"},
    {130, "SOURCEPLAYER_EQUIPMENT_ACTIVATE", "LL"},
    {131, "SOURCEPLAYER_FOOT_OFFSETS", "DDDD"},
    {132, "SOURCEPLAYER_DAMAGE_MODIFIER", "DD"},
    {133, "SOURCEPLAYER_DETECTOR", "L"},
    {134, "SOURCEPLAYER_TARGETING", "L"},
    {135, "SOURCEPLAYER_TARGETED", "L"},
    {136, "SOURCEPLAYER_EQUIPMENT_INDEX", "L"},
    {137, "SOURCEPLAYER_WEAPON_INDEX", "L"},
    {138, "SOURCEPLAYER_WAKE", "L"},
    {139, "SOURCEPLAYER_KILL", nullptr},
    {140, "SOURCEPLAYER_SELL_ALL", nullptr},
    {141, "SOURCEPLAYER_DISABLE", "E"},
    {142, "SOURCEPLAYER_ENABLE", "E"},
    {143, "SOURCEPLAYER_FOCAL_ADJUST", "D"},
    {144, "SOURCEPLAYER_STAND_GROUND", "P"},
    {145, "SOURCEPLAYER_FALL_PITCH_DOWN", "DD"},
    {146, "SOURCEPLAYER_RUB_SPEED_ADJUST", "D"},
    {147, "SOURCEPLAYER_PLAY_LOOP", "LNVLLD"},
    {148, "SOURCEPLAYER_STOP_LOOP", "L"},
    {149, "SOURCEPLAYER_STRAFE", "DPD"},
    {150, "SOURCEPLAYER_Z_STRAFE", "DPD"},
    {151, "SOURCEPLAYER_TARGET_LAG", "P"},
    {152, "SOURCEPLAYER_MOVEMENT_TYPE", "Y"},
    {153, "SOURCEPLAYER_SLEEP", nullptr},
    {154, "SOURCEPLAYER_DELETE", nullptr},
    {155, "SOURCEPLAYER_MOVE_SPEED", "D"},
    {156, "SOURCEPLAYER_STEALTH", "L"},
    {157, "SOURCEPLAYER_TARGETED_MODIFIERS", "DD"},
    {159, "SOURCEPLAYER_PATH_INDEX", "L"},
    {160, "AND_SOURCEPLAYER_BEHAVIOUR_BITS", "L"},
    {161, "OR_SOURCEPLAYER_BEHAVIOUR_BITS", "L"},
    {162, "AND_SOURCEPLAYER_EXTRA_BITS", "L"},
    {163, "OR_SOURCEPLAYER_EXTRA_BITS", "L"},
    {164, "THING_ACCURACY_MODIFIERS", "PDD"},
    {165, "THING_TARGET_PLAYERS_MODIFIERS", "DD"},
    {166, "FREEZE", "P"},
    {167, "REFRESH_SCREEN", nullptr},
    {168, "SET_DEATHMATCH_START", "LLLLAAA"},
    {169, "SET_PLAYER_START", "LLLLAAA"},
    {173, "DEFINE_VIRUS", "LTNVLD"},
    {174, "DEFINE_REGENERATION_MARKER", "ODD"},
    {176, "DEFINE_SHIELDS", "LL"},
    {177, "ADD_RANDOM_AMMO_ITEM_AT_SOURCE", "P"},
    {178, "ADJUST_SOURCEPLAYER_MORALE", "L"},
    {179, "FILM_DEFINE", "LPLS"},
    {180, "FILM_PLAY", nullptr},
    {181, "FILM_STOP", nullptr},
    {182, "FILM_REWIND", nullptr},
    {183, "ADD_SWITCH", "LELLLLVN"},
    {184, "ADD_DOOR", "LEEEEEAAALLLLLLLDDDDDDDDDVNVNVN"},
    {185, "ADD_NAMED_ITEM", "ILLLAAA"},
    {186, "ADD_NAMED_ITEM_EX", "LLLILLLAAA"},
    {187, "ADD_SOUND_SOURCE", "LLLNVD"},
    {189, "SHOW_AMMO_BUY_SCREEN", nullptr},
    {190, "SHOW_ITEM_BUY_SCREEN", nullptr},
    {191, "SHOW_TANK_BUY_SCREEN", nullptr},
    {192, "SHOW_TANK_SAVE_SCREEN", nullptr},
    {193, "SHOW_WEAPON_BUY_SCREEN", nullptr},
    {194, "SHOW_BRIEFING_SCREEN", nullptr},
    {195, "SHOW_HELP_SCREEN", nullptr},
    {196, "ADD_LOCATION", "ELLLAAA"},
    {197, "DELETE_LOCATION", "E"},
    {198, "STOP_SOURCEPLAYER_SLEEPING_SCRIPTS", nullptr},
    {202, "SET_SOURCEPLAYER_TYPE", "L"},
};

constexpr CommandDef kConditions[] = {
    {1, "ADD_THING", "GLLLAAA"},
    {2, "ADD_THING_EX", "LLLGLLLAAAL"},
    {3, "ADD_THING_AT_SOURCE", "LGAAA"},
    {4, "ADD_THING_AT_SOURCE_WEAPON_POINT", "LLGDDDD"},
    {5, "ADD_THING_RELATIVE", "LGLLLAAADDDD"},
    {6, "GAME_DIFFICULTY_AT_LEAST", "L"},
    {7, "LEVEL_OBJECTIVES_COMPLETE", nullptr},
    {8, "NEXT_LEVEL_NUMBER", "L"},
    {9, "SOURCE_IS_PLAYER", nullptr},
    {10, "SOURCE_IS_LOCAL_PLAYER_ONE", nullptr},
    {11, "SOURCE_IS_LOCAL_PLAYER_TWO", nullptr},
    {12, "DEATHMATCH_GAME", nullptr},
    {13, "DEATHMATCH_CLASSIC_GAME", nullptr},
    {14, "DEATHMATCH_NORMAL_GAME", nullptr},
    {15, "MULTIPLAYER_GAME", nullptr},
    {16, "CAPTURE_THE_FLAG_GAME", nullptr},
    {17, "TEAM_GAME", nullptr},
    {18, "FIRE_SOURCEPLAYER_BULLET", "LELBBLAAL"},
    {19, "FIRE_SOURCEPLAYER_LASER", "TMLELBBLAAD"},
    {20, "FIRE_SOURCEPLAYER_THING", "LLGLLLAAADAAAAD"},
    {21, "NEAREST_OBSTRUCTION", "LLLL"},
    {22, "NEAREST_OBSTRUCTION_FROM_SOURCE", "L"},
    {23, "PLAYERS_AT_LEAST", "L"},
    {24, "RANDOM", "D"},
    {25, "RANDOM_FRAME_BALANCED", "D"},
    {26, "GOD_MODE", nullptr},
    {27, "SOURCE_SWAP", nullptr},
    {28, "SCAVENGERHUNT_GAME", nullptr},
    {29, "SOURCE_FROM_GROUND", "D"},
    {30, "SOURCEPLAYER_KEY_BITS_SET", "L"},
    {31, "SOURCEPLAYER_WEAPON_BITS_SET", "L"},
    {32, "SOURCEPLAYER_AMMO_FULL", "L"},
    {33, "SOURCEPLAYER_AMMO_AT_LEAST", "LL"},
    {34, "SOURCEPLAYER_CASH_AT_LEAST", "L"},
    {35, "SOURCEPLAYER_EXTRA_BITS_SET", "L"},
    {36, "SOURCEPLAYER_IS", "L"},
    {37, "SOURCEPLAYER_SHIELD_FULL", nullptr},
    {38, "SOURCEPLAYER_LIFE_AT_LEAST", "D"},
    {39, "SOURCEPLAYER_LIFE_FULL", nullptr},
    {40, "SOURCEPLAYER_HAS_WEAPON_FOR_AMMO", "L"},
    {41, "SOURCEPLAYER_TELEPORT_HANDLE", "E"},
    {42, "SOURCEPLAYER_TELEPORT", "DDDAAA"},
    {43, "SOURCEPLAYER_TYPE", "L"},
    {44, "SOURCEPLAYER_THING_TYPE", "S"},
    {45, "SOURCEPLAYER_WARP", "P"},
    {46, "SOURCEPLAYER_WEAPON_POWER_UP", "D"},
    {47, "SOURCEPLAYER_UNDERWATER", nullptr},
    {48, "SOURCE_UNDERWATER", nullptr},
    {49, "SOURCEPLAYER_SECTOR_FLAGS", "L"},
    {50, "SOURCEPOLY_HANDLE", "E"},
    {51, "SOURCESECTOR_FLAGS", "L"},
    {52, "CUSTOM_COMMAND_0", nullptr},
    {53, "CUSTOM_COMMAND_1", "L"},
    {54, "CUSTOM_COMMAND_2", "LL"},
    {55, "CUSTOM_COMMAND_3", "LLL"},
    {56, "CUSTOM_COMMAND_4", "LLLL"},
    {57, "CUSTOM_COMMAND_5", "LLLLL"},
    {58, "CUSTOM_COMMAND_6", "LLLLLL"},
    {59, "ADD_OTHER", "ZLLLAAA"},
};

template <std::size_t N>
const CommandDef* find_def(const CommandDef (&defs)[N], std::uint8_t id) {
    for (const auto& def : defs) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Byte reader.  `pos` is never allowed past the end of the code block.
// ---------------------------------------------------------------------------

struct Reader {
    std::span<const std::uint8_t> code;
    std::size_t pos = 0;
    std::string* error = nullptr;

    bool fail(std::string message) const {
        if (error != nullptr && error->empty()) {
            *error = std::move(message);
        }
        return false;
    }

    bool need(std::size_t n) const {
        return n <= code.size() - pos;
    }

    bool u8(std::uint8_t& out) {
        if (!need(1)) {
            return fail("truncated operand at 0x" + hex(pos));
        }
        out = code[pos++];
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (!need(4)) {
            return fail("truncated u32 at 0x" + hex(pos));
        }
        out = rd32(code.data() + pos);
        pos += 4;
        return true;
    }

    bool u64(std::uint64_t& out) {
        if (!need(8)) {
            return fail("truncated u64 at 0x" + hex(pos));
        }
        out = rd64(code.data() + pos);
        pos += 8;
        return true;
    }

    bool skip(std::size_t n) {
        if (!need(n)) {
            return fail("truncated block at 0x" + hex(pos));
        }
        pos += n;
        return true;
    }

    bool str(std::string& out) {
        const std::size_t begin = pos;
        while (pos < code.size() && code[pos] != 0) {
            ++pos;
        }
        if (pos == code.size()) {
            return fail("unterminated string at 0x" + hex(begin));
        }
        out.assign(reinterpret_cast<const char*>(code.data() + begin), pos - begin);
        ++pos;
        return true;
    }

    static std::string hex(std::size_t value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%zx", value);
        return buf;
    }
};

bool letter_info(char letter, std::size_t& size, bool& is_string) {
    is_string = false;
    switch (letter) {
    case 'A':
    case 'B':
    case 'D':
        size = 8;
        return true;
    case 'Y':
        size = 1;
        return true;
    case 'S':
        is_string = true;
        size = 0;
        return true;
    case 'E':
    case 'G':
    case 'I':
    case 'L':
    case 'M':
    case 'N':
    case 'O':
    case 'P':
    case 'R':
    case 'T':
    case 'V':
    case 'Z':
        size = 4;
        return true;
    default:
        return false;
    }
}

bool read_args(Reader& r, std::string_view sig, std::vector<SbtValue>& out) {
    out.reserve(out.size() + sig.size());
    for (const char letter : sig) {
        std::size_t size = 0;
        bool is_string = false;
        if (!letter_info(letter, size, is_string)) {
            return r.fail("bad operand signature letter");
        }
        if (is_string) {
            std::string text;
            if (!r.str(text)) {
                return false;
            }
            out.push_back(SbtValue::from_string(std::move(text)));
        } else if (size == 8) {
            std::uint64_t bits = 0;
            if (!r.u64(bits)) {
                return false;
            }
            out.push_back(SbtValue::from_real(std::bit_cast<double>(bits)));
        } else {
            std::uint32_t value = 0;
            if (size == 1) {
                std::uint8_t byte = 0;
                if (!r.u8(byte)) {
                    return false;
                }
                value = byte;
            } else if (!r.u32(value)) {
                return false;
            }
            out.push_back(SbtValue::from_uint(value));
        }
    }
    return true;
}

// Condition operand list, shared by IF (199) and EVAL (0xC9).
bool read_condition(Reader& r, SbtInsn& insn) {
    std::uint8_t cid = 0;
    if (!r.u8(cid)) {
        return false;
    }
    if (const CommandDef* def = find_def(kConditions, cid)) {
        insn.condition = def->name;
        if (def->sig != nullptr && !read_args(r, def->sig, insn.args)) {
            return false;
        }
    } else {
        insn.condition = "OP_" + std::to_string(cid);
    }
    return true;
}

// DEFINE_WEAPONS / DEFINE_AMMO / DEFINE_PLAYER_TYPES / DEFINE_EQUIPMENT.
bool read_table(Reader& r, std::string_view sig, SbtInsn& insn) {
    std::size_t string_count = 0;
    while (string_count < sig.size() && sig[string_count] == 'S') {
        ++string_count;
    }

    std::uint32_t count = 0;
    if (!r.u32(count)) {
        return false;
    }
    // Every record consumes at least one byte (string terminator or operand),
    // so this bound also protects the loop from a corrupt count.
    if (count > r.code.size()) {
        return r.fail("table count too large at 0x" + Reader::hex(r.pos));
    }

    insn.table.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) {
        SbtTableRecord record;
        record.strings.reserve(string_count);
        for (std::size_t s = 0; s < string_count; ++s) {
            std::string text;
            if (!r.str(text)) {
                return false;
            }
            record.strings.push_back(std::move(text));
        }
        if (!read_args(r, sig.substr(string_count), record.values)) {
            return false;
        }
        insn.table.push_back(std::move(record));
    }
    insn.count = count;
    return true;
}

bool decode_insns(std::span<const std::uint8_t> code, std::size_t start, std::size_t end,
                  std::vector<SbtInsn>& out, std::string& error) {
    if (start > end || end > code.size()) {
        error = "bad instruction range";
        return false;
    }

    Reader r{code, start, &error};
    while (r.pos < end) {
        const auto offset = static_cast<std::uint32_t>(r.pos);
        const std::uint8_t opcode = code[r.pos];

        if (opcode == 0xFF) {
            ++r.pos;
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "END";
            out.push_back(std::move(insn));
            continue;
        }

        if (opcode == 170 || opcode == 171 || opcode == 172 || opcode == 175) {
            static constexpr const char* kTableNames[] = {
                "DEFINE_WEAPONS", "DEFINE_AMMO", "DEFINE_PLAYER_TYPES", "DEFINE_EQUIPMENT"};
            static constexpr const char* kTableSigs[] = {
                "SSSLLLLEEEELLLLDBBDL",
                "SLEOLLLLLLL",
                "SSSLDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE",
                "SLEEEEEELLLLL"};
            const std::size_t which = opcode == 170 ? 0 : (opcode == 171 ? 1 : (opcode == 172 ? 2 : 3));
            ++r.pos;
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = kTableNames[which];
            if (!read_table(r, kTableSigs[which], insn)) {
                return false;
            }
            out.push_back(std::move(insn));
            continue;
        }

        if (opcode == 103) { // DEFINE_LEVEL_OBJECTIVES
            ++r.pos;
            std::uint32_t count = 0;
            if (!r.u32(count)) {
                return false;
            }
            if (count > 30) {
                error = "DEFINE_LEVEL_OBJECTIVES count too large (" + std::to_string(count) + ")";
                return false;
            }
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "DEFINE_LEVEL_OBJECTIVES";
            insn.count = count;
            insn.args.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                std::string text;
                if (!r.str(text)) {
                    return false;
                }
                insn.args.push_back(SbtValue::from_string(std::move(text)));
            }
            out.push_back(std::move(insn));
            continue;
        }

        if (opcode == 158) { // SOURCEPLAYER_PATH
            ++r.pos;
            std::uint32_t count = 0;
            if (!r.u32(count)) {
                return false;
            }
            if (count > 32) {
                error = "SOURCEPLAYER_PATH point count too large (" + std::to_string(count) + ")";
                return false;
            }
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "SOURCEPLAYER_PATH";
            insn.count = count;
            insn.ints.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                std::uint32_t point = 0;
                if (!r.u32(point)) {
                    return false;
                }
                insn.ints.push_back(point);
            }
            out.push_back(std::move(insn));
            continue;
        }

        if (opcode == 188) { // ADD_LIFT: 0x94-byte head + stops of 3 dwords
            ++r.pos;
            if (!r.skip(0x94)) {
                return false;
            }
            std::uint32_t count = 0;
            if (!r.u32(count)) {
                return false;
            }
            if (count > (code.size() - r.pos) / 12) {
                error = "ADD_LIFT stop count too large (" + std::to_string(count) + ")";
                return false;
            }
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "ADD_LIFT";
            insn.count = count;
            insn.ints.reserve(static_cast<std::size_t>(count) * 3);
            for (std::uint32_t k = 0; k < count * 3; ++k) {
                std::uint32_t word = 0;
                if (!r.u32(word)) {
                    return false;
                }
                insn.ints.push_back(word);
            }
            out.push_back(std::move(insn));
            continue;
        }

        if (opcode == 199) { // IF <cond> <size> { body }
            ++r.pos;
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "IF";
            if (!read_condition(r, insn)) {
                return false;
            }
            const std::size_t size_offset = r.pos;
            std::uint32_t size = 0;
            if (!r.u32(size)) {
                return false;
            }
            const std::size_t body_start = size_offset + 4;
            const std::size_t body_end = size_offset + size;
            if (body_end < body_start || body_end > code.size()) {
                error = "IF body past end of code at 0x" + Reader::hex(offset);
                return false;
            }
            insn.size = size;
            if (!decode_insns(code, body_start, body_end, insn.body, error)) {
                return false;
            }
            out.push_back(std::move(insn));
            r.pos = body_end;

            // An ELSE clause (opcode 200) always immediately follows the
            // then-branch; its relative jump lands after the else-branch.
            if (r.pos < end && code[r.pos] == 200) {
                const auto else_offset = static_cast<std::uint32_t>(r.pos);
                ++r.pos;
                std::uint32_t rel = 0;
                if (!r.u32(rel)) {
                    return false;
                }
                const std::size_t target = static_cast<std::size_t>(else_offset) + 1 + rel;
                if (target > code.size() || target < r.pos) {
                    error = "bad ELSE jump at 0x" + Reader::hex(else_offset);
                    return false;
                }
                SbtInsn clause;
                clause.offset = else_offset;
                clause.opcode = 200;
                clause.name = "ELSE";
                clause.target = static_cast<std::uint32_t>(target);
                clause.size = rel;
                clause.has_body = true;
                if (!decode_insns(code, r.pos, target, clause.body, error)) {
                    return false;
                }
                out.push_back(std::move(clause));
                r.pos = target;
            }
            continue;
        }

        if (opcode == 200) { // standalone ELSE / relative jump
            ++r.pos;
            std::uint32_t rel = 0;
            if (!r.u32(rel)) {
                return false;
            }
            const std::size_t target = static_cast<std::size_t>(offset) + 1 + rel;
            if (target > code.size()) {
                error = "bad ELSE jump at 0x" + Reader::hex(offset);
                return false;
            }
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "ELSE";
            insn.target = static_cast<std::uint32_t>(target);
            insn.size = rel;
            out.push_back(std::move(insn));
            r.pos = target;
            continue;
        }

        if (opcode == 0xC9) { // bare condition statement
            ++r.pos;
            SbtInsn insn;
            insn.offset = offset;
            insn.opcode = opcode;
            insn.name = "EVAL";
            if (!read_condition(r, insn)) {
                return false;
            }
            out.push_back(std::move(insn));
            continue;
        }

        const CommandDef* def = find_def(kCommands, opcode);
        if (def == nullptr) {
            error = "unknown opcode " + std::to_string(opcode) + " at 0x" + Reader::hex(offset);
            return false;
        }
        ++r.pos;
        SbtInsn insn;
        insn.offset = offset;
        insn.opcode = opcode;
        insn.name = def->name;
        if (def->sig != nullptr && !read_args(r, def->sig, insn.args)) {
            return false;
        }
        out.push_back(std::move(insn));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Text form (must stay byte-identical to gm/sbtvm.py's render()).
// ---------------------------------------------------------------------------

std::string format_value(const SbtValue& value) {
    switch (value.kind) {
    case SbtValue::Kind::String:
        return "\"" + value.s + "\"";
    case SbtValue::Kind::Real: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", value.d);
        return buf;
    }
    case SbtValue::Kind::UInt:
    default:
        if (value.u >= 0x10000) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%u(0x%x)", value.u, value.u);
            return buf;
        }
        return std::to_string(value.u);
    }
}

std::string join_values(const std::vector<SbtValue>& values, const char* separator) {
    std::string out;
    for (std::size_t k = 0; k < values.size(); ++k) {
        if (k != 0) {
            out += separator;
        }
        out += format_value(values[k]);
    }
    return out;
}

std::string pad_right(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

bool is_table_name(const std::string& name) {
    return name == "DEFINE_WEAPONS" || name == "DEFINE_AMMO" ||
           name == "DEFINE_PLAYER_TYPES" || name == "DEFINE_EQUIPMENT";
}

void render_insns(const std::vector<SbtInsn>& insns, std::size_t indent,
                  std::vector<std::string>& lines) {
    const std::string pad(4 * indent, ' ');
    for (std::size_t k = 0; k < insns.size(); ++k) {
        const SbtInsn& insn = insns[k];

        if (insn.name == "END") {
            lines.push_back(pad + "---");
        } else if (is_table_name(insn.name)) {
            lines.push_back(pad + insn.name + " " + std::to_string(insn.table.size()) +
                            " entries");
            for (const SbtTableRecord& record : insn.table) {
                std::string strings;
                for (std::size_t s = 0; s < record.strings.size(); ++s) {
                    if (s != 0) {
                        strings += " | ";
                    }
                    strings += record.strings[s];
                }
                lines.push_back(pad + "    " + pad_right(strings, 24) + " " +
                                join_values(record.values, " "));
            }
        } else if (insn.name == "DEFINE_LEVEL_OBJECTIVES") {
            std::string line = pad + "DEFINE_LEVEL_OBJECTIVES ";
            for (std::size_t a = 0; a < insn.args.size(); ++a) {
                if (a != 0) {
                    line += " ";
                }
                line += "\"" + insn.args[a].s + "\"";
            }
            lines.push_back(std::move(line));
        } else if (insn.name == "SOURCEPLAYER_PATH") {
            std::string points;
            for (std::size_t p = 0; p < insn.ints.size(); ++p) {
                if (p != 0) {
                    points += " ";
                }
                points += format_value(SbtValue::from_uint(insn.ints[p]));
            }
            lines.push_back(pad + "SOURCEPLAYER_PATH n=" + std::to_string(insn.count) + " " +
                            points);
        } else if (insn.name == "ADD_LIFT") {
            lines.push_back(pad + "ADD_LIFT n=" + std::to_string(insn.count));
        } else if (insn.name == "IF") {
            std::string head = pad + "IF " + insn.condition;
            if (!insn.args.empty()) {
                head += " " + join_values(insn.args, " ");
            }
            lines.push_back(head + " {");
            render_insns(insn.body, indent + 1, lines);
            const bool has_else = k + 1 < insns.size() &&
                                  insns[k + 1].name == "ELSE" && insns[k + 1].has_body;
            if (has_else) {
                lines.push_back(pad + "} else {");
                render_insns(insns[k + 1].body, indent + 1, lines);
                ++k;
            }
            lines.push_back(pad + "}");
        } else if (insn.name == "ELSE") {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%sELSE -> 0x%x", pad.c_str(), insn.target);
            lines.push_back(buf);
        } else if (insn.name == "EVAL") {
            lines.push_back(pad + "EVAL " + insn.condition + " " +
                            join_values(insn.args, " "));
        } else {
            std::string line = pad + pad_right(insn.name, 34);
            if (!insn.args.empty()) {
                line += " " + join_values(insn.args, " ");
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    }
}

} // namespace

SbtValue SbtValue::from_uint(std::uint32_t value) {
    SbtValue out;
    out.kind = Kind::UInt;
    out.u = value;
    return out;
}

SbtValue SbtValue::from_real(double value) {
    SbtValue out;
    out.kind = Kind::Real;
    out.d = value;
    return out;
}

SbtValue SbtValue::from_string(std::string value) {
    SbtValue out;
    out.kind = Kind::String;
    out.s = std::move(value);
    return out;
}

std::optional<SbtProgram> decode_sbt_script(std::span<const std::uint8_t> code,
                                           std::string& error) {
    error.clear();
    SbtProgram script;
    if (!decode_insns(code, 0, code.size(), script.insns, error)) {
        return std::nullopt;
    }
    script.end = code.size();
    return script;
}

std::string render_sbt_script(const SbtProgram& script) {
    std::vector<std::string> lines;
    render_insns(script.insns, 0, lines);
    std::string out;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (k != 0) {
            out += '\n';
        }
        out += lines[k];
    }
    return out;
}

} // namespace gm
