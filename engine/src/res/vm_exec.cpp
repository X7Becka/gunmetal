// SCRIPT-VM executor: C++ port of gm/sbtvm_exec.py (docs/VM_EXEC.md).
//
// The engine has no operand stack: the loop dispatches on one opcode at a byte
// PC, IF/ELSE move the PC, and conditions are individual handlers that compare
// their inline operands with engine state (table 0x45f2ac).  This file keeps
// that model verbatim -- byte-exact decoding on demand, a GENERATE sub-script
// stack, an explicit state and deterministic limits -- so the C++ results can
// be compared with the Python reference instruction for instruction.
#include "res/vm_exec.h"

#include "core/bytes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gm {
namespace {

// ---------------------------------------------------------------------------
// Command / condition tables.  Copied from gm/sbtvm.py (recovered from
// gundx.exe at 0x4b53e8 / 0x4b62ec and 0x45f2ac); sbtvm.cpp keeps its own copy
// because it only needs the text form and must not grow a second API.
// One letter per operand: A/B/D = 8-byte double, Y = 1 byte, S = C string,
// every other letter = 4 bytes.
// ---------------------------------------------------------------------------

struct Def {
    std::uint8_t id;
    const char* name;
    const char* sig; // nullptr: no operands
};

constexpr Def kCommands[] = {
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

constexpr Def kConditions[] = {
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

//: DEFINE_* record tables: opcode -> (name, one-record signature, count limit).
struct TableDef {
    std::uint8_t id;
    const char* name;
    const char* sig;
    std::uint32_t limit;
};

constexpr TableDef kTables[] = {
    {170, "DEFINE_WEAPONS", "SSSLLLLEEEELLLLDBBDL", 31},
    {171, "DEFINE_AMMO", "SLEOLLLLLLL", 31},
    {172, "DEFINE_PLAYER_TYPES", "SSSLDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE", 32},
    {175, "DEFINE_EQUIPMENT", "SLEEEEEELLLLL", 31},
};

template <std::size_t N>
const Def* find_def(const Def (&defs)[N], std::string_view name) {
    for (const auto& def : defs) {
        if (name == def.name) {
            return &def;
        }
    }
    return nullptr;
}

template <std::size_t N>
const Def* find_def(const Def (&defs)[N], std::uint8_t id) {
    for (const auto& def : defs) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

const TableDef* find_table(std::uint8_t id) {
    for (const auto& def : kTables) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

//: Structural opcodes with no COMMANDS entry (docs/FORMATS.md 12.3).
bool is_structural(std::uint8_t op) {
    switch (op) {
    case 103:
    case 158:
    case 170:
    case 171:
    case 172:
    case 175:
    case 188:
        return true;
    default:
        return false;
    }
}

// Classification sets, verbatim from gm/sbtvm_exec.py.
const std::unordered_set<std::string_view> kFlow = {
    "EXIT_EVENT", "EXIT_PROGRAM", "SLEEP", "GENERATE",
    "NEW_GAME", "NEW_LEVEL", "NEXT_LEVEL", "END_GAME",
};

const std::unordered_set<std::string_view> kSceneCommands = {
    "ADD_SKY_LIGHT_SOURCE", "ADD_LIGHT_SOURCE", "ADD_LOCATION",
    "ADD_ITEM", "ADD_ITEM_FIXED", "ADD_SOUND_SOURCE", "ADD_SPRITE",
    "ADD_DOOR", "ADD_SWITCH", "SET_PLAYER_START", "SET_DEATHMATCH_START",
    "ADD_NAMED_ITEM", "ADD_NAMED_ITEM_EX", "DELETE_LOCATION",
};

const std::unordered_set<std::string_view> kAttached = {
    "ADD_ITEM_AT_SOURCE", "ADD_ITEM_AT_SOURCE_FIXED",
    "ADD_SPRITE_AT_SOURCE", "ADD_SPRITE_AT_SOURCE_WEAPON_POINT",
    "ADD_SPRITE_MOVING", "ADD_SPRITE_MOVING_AT_SOURCE",
    "ADD_SPRITE_MOVING_AT_SOURCEPLAYER",
    "ADD_SPRITE_MOVING_AT_SOURCE_WEAPON_POINT",
    "ADD_SOURCEPLAYER_SPRITE", "ADD_RANDOM_AMMO_ITEM_AT_SOURCE",
};

const std::unordered_set<std::string_view> kState = {
    "ENABLE", "DISABLE", "SOURCEPLAYER_ENABLE", "SOURCEPLAYER_DISABLE",
    "OR_LEVEL_OBJECTIVES_BITS",
    "AND_SOURCEPLAYER_KEY_BITS", "OR_SOURCEPLAYER_KEY_BITS",
    "AND_SOURCEPLAYER_WEAPON_BITS", "OR_SOURCEPLAYER_WEAPON_BITS",
    "AND_SOURCEPLAYER_EQUIPMENT_BITS", "OR_SOURCEPLAYER_EQUIPMENT_BITS",
    "AND_SOURCEPLAYER_BEHAVIOUR_BITS", "OR_SOURCEPLAYER_BEHAVIOUR_BITS",
    "AND_SOURCEPLAYER_EXTRA_BITS", "OR_SOURCEPLAYER_EXTRA_BITS",
    "ADD_SOURCEPLAYER_AMMO", "ADD_SOURCEPLAYER_CASH",
    "ADD_SOURCEPLAYER_SALVAGE", "ADJUST_SOURCEPLAYER_MORALE",
    "HEAL_SOURCEPLAYER", "REPAIR_SOURCEPLAYER",
    "INCREMENT_SOURCEPLAYER_SECRET_TALLY",
    "SOURCEPLAYER_EQUIPMENT_ACTIVATE", "SOURCEPLAYER_EQUIPMENT_INDEX",
    "SOURCEPLAYER_WEAPON_INDEX", "SOURCEPLAYER_TYPE", "SET_SOURCEPLAYER_TYPE",
    "SOURCEPLAYER_MOVEMENT_TYPE",
    "SET_LEVEL_NAME", "SET_LEVEL_GRAVITY", "SET_LEVEL_FOG_COLOUR",
    "SET_LEVEL_FOG_DISTANCE", "SET_LEVEL_COUNTDOWN_TIMER",
    "SET_LEVEL_FOCAL_ADJUST", "SET_LEVEL_ALIASING_VALUES",
    "SET_LEVEL_VERSION", "SET_LEVEL_BEST_TIME", "SET_LEVEL_SECRETS",
    "SET_LEVEL_END_CAMERA_XYZ",
    "DEFINE_SHIELDS", "DEFINE_VIRUS", "DEFINE_REGENERATION_MARKER",
    "SOURCE_CLEAR", "SOURCEPLAYER_KILL",
    // SECTOR_STUFF (0x800b60): the sector gameplay table.
    "SET_SECTOR_MOVEMENT", "SET_SECTOR_DAMAGE", "SET_SECTOR_SOUND",
    "SET_SECTOR_EVENTS", "SET_SECTOR_FLAGS",
    "SET_SOURCESECTOR_MOVEMENT", "SET_SOURCESECTOR_FLAGS", "SET_SOURCESECTOR_DAMAGE",
    "SET_HANDLESECTOR_MOVEMENT", "SET_HANDLESECTOR_FLAGS", "SET_HANDLESECTOR_DAMAGE",
    "SET_HANDLESECTOR_SOUND",
    // The world sector records (0x5670d8): light (+0x16) and type (+0x14).
    "SET_SECTOR_BRIGHTNESS", "SET_SECTOR_TYPE",
    "SET_SOURCESECTOR_BRIGHTNESS", "SET_SOURCESECTOR_TYPE",
    "SET_HANDLESECTOR_BRIGHTNESS", "SET_HANDLESECTOR_TYPE",
    // The world polygon records (0x80 bytes): texture, material, scroll, light.
    "SET_POLY_TEXTURE", "SET_POLY_BRIGHTNESS", "SET_POLY_SCROLL",
    "SET_SOURCEPOLY_TEXTURE", "SET_SOURCEPOLY_BRIGHTNESS",
    "SET_SOURCEPOLY_MATERIAL", "SET_SOURCEPOLY_SCROLL",
    "SET_HANDLEPOLY_TEXTURE", "SET_HANDLEPOLY_BRIGHTNESS",
    "SET_HANDLEPOLY_SCROLL", "SET_HANDLEPOLY_MATERIAL",
};

const std::unordered_set<std::string_view> kEvent = {
    "LOG_MESSAGE", "PLAY_SAMPLE",
    "PLAY_SAMPLE_AT_LOCATION", "SPEAK_NUMBER", "SPEAK_SAMPLE",
    "SET_SOURCEWORLDOBJECT_HIT_OFF", "SET_SOURCEWORLDOBJECT_HIT_ON",
    "SET_SOURCEWORLDOBJECT_TEXTURES",
    "SET_SOURCEPOLY_SWITCH_EVENT", "PLAY_SAMPLE_AT_SOURCE",
    "PLAY_FRAME_LOOP_SAMPLE_AT_SOURCE", "REMOVE_SOURCE_ITEM",
    "ADD_SCREEN_TEXT", "ADD_SOURCESCREEN_TEXT", "SET_DOOR_DELTAS",
    "SET_DOOR_POSITION", "SET_DOOR_SOUNDS", "SET_DOOR_SPEED",
    "ITEMS_MOVE_CHECK", "SET_SOURCEITEM_TRIGGER_EVENT", "SOURCE_CLEAR",
    "SET_CAMERA_PITCH_SHAKE", "SET_CAMERA_ROLL_SHAKE", "SET_CAMERA_YAW_SHAKE",
    "SET_CAMERA_HAZE", "LOAD_MUSIC", "PLAY_MUSIC", "STOP_MUSIC",
    "UNLOAD_MUSIC", "MEMORIZE_CURRENT_MUSIC_BANK",
    "PLAY_MEMORIZED_MUSIC_BANK", "SET_SECTOR_LINK", "SHOW_LEVEL_END_SCREEN",
    "INCREMENT_SOURCEPLAYER_SECRET_TALLY", "SET_LEVEL_COUNTDOWN_TIMER",
    "FREEZE", "REFRESH_SCREEN", "FILM_DEFINE", "FILM_PLAY", "FILM_STOP",
    "FILM_REWIND", "SHOW_AMMO_BUY_SCREEN", "SHOW_ITEM_BUY_SCREEN",
    "SHOW_TANK_BUY_SCREEN", "SHOW_TANK_SAVE_SCREEN",
    "SHOW_WEAPON_BUY_SCREEN", "SHOW_BRIEFING_SCREEN", "SHOW_HELP_SCREEN",
    "STOP_SOURCEPLAYER_SLEEPING_SCRIPTS",
    "SET_SOURCEPLAYER_TYPE",
    "GENERATE_FOR_CHILDREN", "GENERATE_FOR_PARENT",
    // gameplay effects on the source player: not modelled, no control flow
    "CHANGE_SOURCEPLAYER_SOB", "SOURCEPLAYER_TURBO_BOOST",
    "SOURCEPLAYER_RECOIL", "SOURCEPLAYER_COLLISION_TYPE", "SOURCEPLAYER_HAZE",
    "SOURCEPLAYER_HAZE_BRIGHTNESS", "SOURCEPLAYER_INVINCIBLE",
    "SOURCEPLAYER_INFECT", "SOURCEPLAYER_CURE",
    "SOURCEPLAYER_HIGHEST_WEAPON_WITH_AMMO",
    "SOURCEPLAYER_NEXT_WEAPON_WITH_AMMO",
    "SOURCEPLAYER_PREVIOUS_WEAPON_WITH_AMMO", "SOURCEPLAYER_FLY",
    "SOURCEPLAYER_HEADLIGHT", "SOURCEPLAYER_FOOT_OFFSETS",
    "SOURCEPLAYER_DAMAGE_MODIFIER", "SOURCEPLAYER_DETECTOR",
    "SOURCEPLAYER_TARGETING", "SOURCEPLAYER_TARGETED",
    "SOURCEPLAYER_WAKE", "SOURCEPLAYER_KILL", "SOURCEPLAYER_SELL_ALL",
    "SOURCEPLAYER_FOCAL_ADJUST", "SOURCEPLAYER_STAND_GROUND",
    "SOURCEPLAYER_FALL_PITCH_DOWN", "SOURCEPLAYER_RUB_SPEED_ADJUST",
    "SOURCEPLAYER_PLAY_LOOP", "SOURCEPLAYER_STOP_LOOP",
    "SOURCEPLAYER_STRAFE", "SOURCEPLAYER_Z_STRAFE",
    "SOURCEPLAYER_TARGET_LAG", "SOURCEPLAYER_SLEEP",
    "SOURCEPLAYER_DELETE", "SOURCEPLAYER_MOVE_SPEED",
    "SOURCEPLAYER_STEALTH", "SOURCEPLAYER_TARGETED_MODIFIERS",
    "SOURCEPLAYER_PATH_INDEX", "SOURCEPLAYER_DISABLE", "SOURCEPLAYER_ENABLE",
    "THING_ACCURACY_MODIFIERS", "THING_TARGET_PLAYERS_MODIFIERS",
    "CAUSE_BLAST_RADIUS_DAMAGE", "REMOVE_SOURCEPLAYER_SPRITE",
    "SOURCEPLAYER_WEAPON_INDEX",
};

bool contains(const std::unordered_set<std::string_view>& set, std::string_view value) {
    return set.find(value) != set.end();
}

// ---------------------------------------------------------------------------
// Diagnostics.  The executor mirrors the Python exceptions: a halt ends one
// path with a reason, a bad operand/PC is reported at the instruction PC.
// ---------------------------------------------------------------------------

struct VmBadPC {
    std::size_t pc = 0;
};

struct VmBadOperand {
    std::size_t pc = 0;
    std::string detail;
};

struct HaltSignal {
    VmReason reason = VmReason::End;
    std::size_t pc = 0;
    std::string detail;
};

std::string hexs(std::size_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%zx", value);
    return buffer;
}

std::string hexs8(unsigned value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%02x", value);
    return buffer;
}

/// Flag bit ``GENERATE`` tests for a known source (0x45fa8a `SHL EDX,CL` with
/// CL = player 0..7 vs 0x45facf `TEST AH,0x1`), and the bit
/// ``SOURCEPLAYER_ENABLE``/``_DISABLE`` touch (0x457c80/0x457c49).
std::uint32_t source_player_bit(const VmState& state) {
    if (!state.source_player || *state.source_player < 0 || *state.source_player > 7) {
        return 0x100u;
    }
    // `1 << player`: bits 0..7 are the eight players, 0x100 is a non-player
    // source (`0x457c80`, docs/ENTITY_FIELDS.md §5); `9 + player` was wrong.
    return 1u << static_cast<unsigned>(*state.source_player);
}

/// The source handle 0x457390 carries in param 9: a player index, or 0xffff
/// for "no source" (``SOURCE_CLEAR`` writes `&0xffff0000 | 0xffff`).
std::uint32_t source_handle(const VmState& state) {
    if (!state.source_player || *state.source_player < 0 || *state.source_player > 7) {
        return 0xFFFFu;
    }
    return static_cast<std::uint32_t>(*state.source_player);
}

// ---------------------------------------------------------------------------
// Operand reader.  `pos` never moves past the end of the block.
// ---------------------------------------------------------------------------

struct Reader {
    std::span<const std::uint8_t> code;
    std::size_t pos = 0;
    std::size_t pc = 0; // instruction PC used in diagnostics

    [[noreturn]] void bad(const std::string& detail) const {
        throw VmBadOperand{pc, detail};
    }

    void need(std::size_t n) const {
        if (n > code.size() || pos > code.size() - n) {
            bad("need " + std::to_string(n) + " bytes at " + hexs(pos));
        }
    }

    std::uint8_t u8() {
        need(1);
        return code[pos++];
    }

    std::uint32_t u32() {
        need(4);
        const std::uint32_t value = rd32(code.data() + pos);
        pos += 4;
        return value;
    }

    std::uint64_t u64() {
        need(8);
        const std::uint64_t value = rd64(code.data() + pos);
        pos += 8;
        return value;
    }

    std::string str() {
        const std::size_t begin = pos;
        while (pos < code.size() && code[pos] != 0) {
            ++pos;
        }
        if (pos == code.size()) {
            bad("unterminated string at " + hexs(begin));
        }
        std::string out(reinterpret_cast<const char*>(code.data() + begin), pos - begin);
        ++pos;
        return out;
    }
};

bool letter_size(char letter, std::size_t& size, bool& is_string) {
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
        size = 0;
        is_string = true;
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

void read_args(Reader& reader, std::string_view sig, std::vector<SbtValue>& out) {
    for (const char letter : sig) {
        std::size_t size = 0;
        bool is_string = false;
        if (!letter_size(letter, size, is_string)) {
            reader.bad(std::string("bad signature letter '") + letter + "'");
        }
        if (is_string) {
            out.push_back(SbtValue::from_string(reader.str()));
        } else if (size == 8) {
            const std::uint64_t bits = reader.u64();
            double value = 0.0;
            static_assert(sizeof(value) == sizeof(bits));
            std::memcpy(&value, &bits, sizeof(value));
            out.push_back(SbtValue::from_real(value));
        } else if (size == 1) {
            out.push_back(SbtValue::from_uint(reader.u8()));
        } else {
            out.push_back(SbtValue::from_uint(reader.u32()));
        }
    }
}

void read_condition(Reader& reader, VmInsn& insn) {
    const std::uint8_t cid = reader.u8();
    if (const Def* def = find_def(kConditions, cid)) {
        insn.condition = def->name;
        if (def->sig != nullptr) {
            read_args(reader, def->sig, insn.cond_args);
        }
    } else {
        insn.condition = "OP_" + std::to_string(cid);
    }
}

VmInsn decode_insn(std::span<const std::uint8_t> code, std::size_t pc) {
    if (pc >= code.size()) {
        throw VmBadPC{pc};
    }

    VmInsn insn;
    insn.pc = static_cast<std::uint32_t>(pc);
    insn.op = code[pc];
    Reader reader{code, pc + 1, pc};

    if (insn.op == 0xFF) {
        insn.name = "END";
        insn.kind = VmInsnKind::End;
        insn.next = pc + 1;
        return insn;
    }

    if (const TableDef* table = find_table(insn.op)) {
        insn.name = table->name;
        insn.kind = VmInsnKind::Table;
        const std::uint32_t count = reader.u32();
        if (count > table->limit) {
            reader.bad("table count " + std::to_string(count) + " > " +
                       std::to_string(table->limit));
        }
        std::size_t strings = 0;
        while (strings < std::strlen(table->sig) && table->sig[strings] == 'S') {
            ++strings;
        }
        insn.table.reserve(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            SbtTableRecord record;
            record.strings.reserve(strings);
            for (std::size_t s = 0; s < strings; ++s) {
                record.strings.push_back(reader.str());
            }
            read_args(reader, std::string_view(table->sig).substr(strings), record.values);
            insn.table.push_back(std::move(record));
        }
        insn.next = reader.pos;
        return insn;
    }

    if (insn.op == 103) { // DEFINE_LEVEL_OBJECTIVES
        insn.name = "DEFINE_LEVEL_OBJECTIVES";
        insn.kind = VmInsnKind::Objectives;
        const std::uint32_t count = reader.u32();
        if (count > 30) {
            reader.bad("objective count " + std::to_string(count) + " > 30");
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            insn.args.push_back(SbtValue::from_string(reader.str()));
        }
        insn.next = reader.pos;
        return insn;
    }

    if (insn.op == 158) { // SOURCEPLAYER_PATH
        insn.name = "SOURCEPLAYER_PATH";
        insn.kind = VmInsnKind::Path;
        const std::uint32_t count = reader.u32();
        if (count > 32) {
            reader.bad("path point count " + std::to_string(count) + " > 32");
        }
        if (count > (code.size() - reader.pos) / 4) {
            reader.bad("need " + std::to_string(4 * count) + " bytes at " +
                       hexs(reader.pos));
        }
        insn.ints.reserve(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            insn.ints.push_back(reader.u32());
        }
        insn.next = reader.pos;
        return insn;
    }

    if (insn.op == 188) { // ADD_LIFT: 0x94-byte head + stops of 3 dwords
        insn.name = "ADD_LIFT";
        insn.kind = VmInsnKind::Lift;
        reader.need(0x94);
        const std::size_t head = reader.pos;
        insn.lift_head.assign(code.begin() + static_cast<std::ptrdiff_t>(head),
                              code.begin() + static_cast<std::ptrdiff_t>(head + 0x94));
        reader.pos += 0x94;
        const std::uint32_t count = reader.u32();
        if (count > 31) {
            reader.bad("lift stop count " + std::to_string(count) + " > 31");
        }
        if (count > (code.size() - reader.pos) / 12) {
            reader.bad("need " + std::to_string(12 * count) + " bytes at " +
                       hexs(reader.pos));
        }
        insn.stops.reserve(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            std::array<std::uint32_t, 3> stop{};
            stop[0] = reader.u32();
            stop[1] = reader.u32();
            stop[2] = reader.u32();
            insn.stops.push_back(stop);
        }
        insn.next = reader.pos;
        return insn;
    }

    if (insn.op == 199) { // IF <condition> <size> { then }
        insn.name = "IF";
        insn.kind = VmInsnKind::If;
        read_condition(reader, insn);
        const std::size_t size_offset = reader.pos;
        const std::uint32_t size = reader.u32();
        const std::uint64_t body_start = static_cast<std::uint64_t>(size_offset) + 4;
        const std::uint64_t body_end = static_cast<std::uint64_t>(size_offset) + size;
        if (size < 4 || body_end > code.size()) {
            reader.bad("IF body size " + hexs(size) + " at " + hexs(size_offset));
        }
        insn.body_start = static_cast<std::size_t>(body_start);
        insn.body_end = static_cast<std::size_t>(body_end);
        insn.next = insn.body_end;
        return insn;
    }

    if (insn.op == 200) { // ELSE / relative jump
        insn.name = "ELSE";
        insn.kind = VmInsnKind::Else;
        const std::uint32_t rel = reader.u32();
        insn.next = pc + 1;
        insn.target = pc + 1 + rel;
        return insn;
    }

    if (insn.op == 0xC9) { // EVAL <condition>
        insn.name = "EVAL";
        insn.kind = VmInsnKind::Eval;
        read_condition(reader, insn);
        insn.next = reader.pos;
        return insn;
    }

    if (const Def* def = find_def(kCommands, insn.op)) {
        insn.name = def->name;
        insn.kind = VmInsnKind::Cmd;
        if (def->sig != nullptr) {
            read_args(reader, def->sig, insn.args);
        }
        insn.next = reader.pos;
        return insn;
    }

    insn.name = "OP_" + std::to_string(insn.op);
    insn.kind = VmInsnKind::Unknown;
    insn.next = pc + 1;
    insn.error = "no command table entry";
    return insn;
}

// ---------------------------------------------------------------------------
// Operand helpers
// ---------------------------------------------------------------------------

int arg_i32(const SbtValue& value) {
    switch (value.kind) {
    case SbtValue::Kind::Real:
        return static_cast<int>(value.d);
    case SbtValue::Kind::String:
        return 0;
    case SbtValue::Kind::UInt:
    default:
        return static_cast<std::int32_t>(value.u);
    }
}

std::uint32_t arg_u32(const SbtValue& value) {
    switch (value.kind) {
    case SbtValue::Kind::Real:
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(value.d));
    case SbtValue::Kind::String:
        return 0;
    case SbtValue::Kind::UInt:
    default:
        return value.u;
    }
}

double arg_f64(const SbtValue& value) {
    switch (value.kind) {
    case SbtValue::Kind::Real:
        return value.d;
    case SbtValue::Kind::String:
        return 0.0;
    case SbtValue::Kind::UInt:
    default:
        return static_cast<double>(value.u);
    }
}

int channel(const SbtValue& value) {
    return arg_i32(value) & 0xFF;
}

std::string value_text(const SbtValue& value) {
    char buffer[64];
    switch (value.kind) {
    case SbtValue::Kind::String:
        return "\"" + value.s + "\"";
    case SbtValue::Kind::Real:
        std::snprintf(buffer, sizeof(buffer), "%g", value.d);
        return buffer;
    case SbtValue::Kind::UInt:
    default:
        return std::to_string(value.u);
    }
}

std::string args_text(const std::vector<SbtValue>& args) {
    std::string out;
    for (std::size_t k = 0; k < args.size(); ++k) {
        if (k != 0) {
            out += ' ';
        }
        out += value_text(args[k]);
    }
    return out;
}

std::string_view condition_signature(const std::string& name) {
    if (const Def* def = find_def(kConditions, name)) {
        return def->sig != nullptr ? std::string_view(def->sig) : std::string_view();
    }
    return {};
}

//: Conditions that mutate the world while computing a value.
const std::unordered_set<int> kActionConditions = {
    1, 2, 3, 4, 5, 18, 19, 20, 27, 41, 42, 45, 46, 52, 53, 54, 55, 56, 57, 58, 59,
};

//: Conditions whose result is proven from the handler (docs/VM_EXEC.md).
const std::unordered_set<int> kKnownConditions = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
    23, 24, 25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 43,
};

int cond_id(const std::string& name) {
    if (const Def* def = find_def(kConditions, name)) {
        return def->id;
    }
    return -1;
}

constexpr std::size_t kMaxDepth = 32;
constexpr std::size_t kMaxEvents = 200000;

} // namespace

// ---------------------------------------------------------------------------
// Public decoding / classification
// ---------------------------------------------------------------------------

std::string_view vm_reason_name(VmReason reason) {
    switch (reason) {
    case VmReason::End:
        return "end";
    case VmReason::Return:
        return "return";
    case VmReason::UnknownOpcode:
        return "unknown-opcode";
    case VmReason::UnsupportedCommand:
        return "unsupported-command";
    case VmReason::UnknownCondition:
        return "unknown-condition";
    case VmReason::StepLimit:
        return "step-limit";
    case VmReason::PathLimit:
        return "path-limit";
    case VmReason::Deadline:
        return "deadline";
    case VmReason::BadOperand:
        return "bad-operand";
    case VmReason::BadPC:
        return "bad-pc";
    case VmReason::StackOverflow:
        return "stack-overflow";
    case VmReason::Recursion:
        return "recursion";
    case VmReason::ProgramExit:
        return "program-exit";
    case VmReason::LevelTransition:
        return "level-transition";
    }
    return "?";
}

bool vm_reason_ok(VmReason reason) {
    switch (reason) {
    case VmReason::End:
    case VmReason::Return:
    case VmReason::ProgramExit:
    case VmReason::LevelTransition:
        return true;
    default:
        return false;
    }
}

std::optional<VmInsn> vm_decode_at(std::span<const std::uint8_t> code, std::size_t pc,
                                   std::string& error) {
    error.clear();
    try {
        return decode_insn(code, pc);
    } catch (const VmBadPC& bad) {
        error = "pc " + hexs(bad.pc) + " outside code [0, " + hexs(code.size()) + ")";
    } catch (const VmBadOperand& bad) {
        error = "bad operand at " + hexs(bad.pc) + ": " + bad.detail;
    }
    return std::nullopt;
}

VmCommandStatus vm_command_status(std::uint8_t op) {
    if (is_structural(op)) {
        return VmCommandStatus::Structural;
    }
    const Def* def = find_def(kCommands, op);
    if (def == nullptr) {
        return VmCommandStatus::Unknown;
    }
    const std::string_view name = def->name;
    if (contains(kFlow, name)) {
        return VmCommandStatus::Flow;
    }
    if (contains(kSceneCommands, name)) {
        return VmCommandStatus::Scene;
    }
    if (contains(kAttached, name)) {
        return VmCommandStatus::Attached;
    }
    if (contains(kState, name)) {
        return VmCommandStatus::State;
    }
    if (contains(kEvent, name)) {
        return VmCommandStatus::Event;
    }
    return VmCommandStatus::Unsupported;
}

std::string_view vm_command_status_name(VmCommandStatus status) {
    switch (status) {
    case VmCommandStatus::Structural:
        return "structural";
    case VmCommandStatus::Flow:
        return "flow";
    case VmCommandStatus::Scene:
        return "scene";
    case VmCommandStatus::Attached:
        return "attached";
    case VmCommandStatus::State:
        return "state";
    case VmCommandStatus::Event:
        return "event";
    case VmCommandStatus::Unsupported:
        return "unsupported";
    case VmCommandStatus::Unknown:
        break;
    }
    return "unknown";
}

std::optional<VmCommandInfo> vm_command_info(std::string_view name) {
    if (const Def* def = find_def(kCommands, name)) {
        return VmCommandInfo{def->id, def->name, def->sig != nullptr ? def->sig : ""};
    }
    return std::nullopt;
}

std::optional<VmCommandInfo> vm_condition_info(std::string_view name) {
    if (const Def* def = find_def(kConditions, name)) {
        return VmCommandInfo{def->id, def->name, def->sig != nullptr ? def->sig : ""};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

std::map<std::string, std::size_t> VmScene::counts() const {
    return {
        {"lights", lights.size()},
        {"locations", locations.size()},
        {"items", items.size()},
        {"named_items", named_items.size()},
        {"attached", attached.size()},
        {"sounds", sounds.size()},
        {"sprites", sprites.size()},
        {"things", things.size()},
        {"doors", doors.size()},
        {"switches", switches.size()},
        {"lifts", lifts.size()},
        {"spawns", spawns.size()},
    };
}

void VmScene::extend(const VmScene& other) {
    lights.insert(lights.end(), other.lights.begin(), other.lights.end());
    locations.insert(locations.end(), other.locations.begin(), other.locations.end());
    items.insert(items.end(), other.items.begin(), other.items.end());
    named_items.insert(named_items.end(), other.named_items.begin(), other.named_items.end());
    attached.insert(attached.end(), other.attached.begin(), other.attached.end());
    sounds.insert(sounds.end(), other.sounds.begin(), other.sounds.end());
    sprites.insert(sprites.end(), other.sprites.begin(), other.sprites.end());
    things.insert(things.end(), other.things.begin(), other.things.end());
    doors.insert(doors.end(), other.doors.begin(), other.doors.end());
    switches.insert(switches.end(), other.switches.begin(), other.switches.end());
    lifts.insert(lifts.end(), other.lifts.begin(), other.lifts.end());
    spawns.insert(spawns.end(), other.spawns.begin(), other.spawns.end());
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

VmPlayer* VmState::player(int index) {
    auto it = players.find(index);
    if (it != players.end()) {
        return &it->second;
    }
    VmPlayer created;
    created.index = index;
    return &players.emplace(index, std::move(created)).first->second;
}

VmPlayer* VmState::player() {
    if (!source_player) {
        return nullptr;
    }
    return player(*source_player);
}

double VmState::rnd() {
    rng = (rng * 1103515245u + 12345u) & 0x7FFFFFFFu;
    return static_cast<double>(rng & 0xFFFu) / 4095.0;
}

VmEntity* VmState::entity(int id) {
    for (VmEntity& row : entities) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

const VmEntity* VmState::entity(int id) const {
    for (const VmEntity& row : entities) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

std::size_t VmState::entity_count(int id) const {
    std::size_t count = 0;
    for (const VmEntity& row : entities) {
        if (row.id == id) {
            ++count;
        }
    }
    return count;
}

VmState vm_state_from_level(const SbtLevel& level, const VmContext& context) {
    VmState state;
    state.code = level.script_code;
    state.game_mode = context.game_mode;
    state.mp_flags = context.mp_flags;
    state.player_count = context.player_count;
    state.difficulty = context.difficulty;
    state.level_number = context.level_number;
    state.mission_counter = context.level_number > 0 ? context.level_number : 1;
    state.god_mode = context.god_mode;
    state.rng = context.seed & 0x7FFFFFFFu;
    state.source_player = context.source_player;

    const int players = std::max(1, context.players);
    for (int i = 0; i < players; ++i) {
        VmPlayer& player = *state.player(i);
        if (context.playertype) {
            player.playertype = *context.playertype;
        }
    }

    const std::size_t n = level.script.handles;
    if (n > 0 && level.script_handles_data.size() >= n * 12) {
        const auto* data = level.script_handles_data.data();
        state.entities.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            VmEntity entity;
            entity.flags = rd32(data + i * 4);
            entity.id = static_cast<std::int32_t>(rd32(data + (n + i) * 4));
            entity.offset = rd32(data + (2 * n + i) * 4);
            state.order.push_back(entity.id);
            // Every row is kept, duplicate ids included: 0x45fa00 runs each of
            // them and ENABLE/DISABLE edit every match (docs/ENTITY_FIELDS §4).
            state.entities.push_back(entity);
        }
    }
    // SECTOR_HANDLE (record 17) is part of the level file: the original's
    // loader 0x463900 fills the count 0x5e3de0, the tags 0x5b1df8 and the
    // sectors 0x617fb8 (cap 0x100), and SET_HANDLESECTOR_* (40..45) scans
    // that table (0x4582bc).  Seed it so those commands resolve on a
    // shipped level instead of being a no-op.
    state.sector_handles.clear();
    state.sector_handles.reserve(std::min<std::size_t>(level.sector_handles.size(), 0x100));
    for (const SbtHandle& row : level.sector_handles) {
        if (state.sector_handles.size() >= 0x100) {
            break;
        }
        state.sector_handles.emplace_back(static_cast<int>(row.tag),
                                          static_cast<int>(row.a));
    }
    // Levels do not carry SECTOR_STUFF (36/36, docs/RECORDS.md §1), but a
    // level loaded from a save stream does; keep the same seeding path.
    vm_load_sector_stuff(state, level.sector_stuff);
    // Nine sleeping tables follow the code when SCRIPT's flag byte is set.
    vm_load_sleeping_scripts(state, level.script_tables);
    return state;
}

void vm_load_sleeping_scripts(VmState& state, std::span<const std::uint8_t> tables) {
    state.sleeping = {};
    if (tables.size() != kVmSleepSlots * 4 * 9) {
        return;
    }
    const auto* data = tables.data();
    // 0x462cxx reads the nine arrays in this order (each 0x400 bytes).
    auto word = [data](std::size_t table, std::size_t slot) {
        return rd32(data + (table * kVmSleepSlots + slot) * 4);
    };
    for (std::size_t i = 0; i < kVmSleepSlots; ++i) {
        VmSleepSlot& slot = state.sleeping[i];
        slot.ticks = static_cast<std::int32_t>(word(0, i));  // 0x5e4278
        slot.offset = static_cast<std::int32_t>(word(1, i)); // 0x5e3df0
        for (std::size_t k = 0; k < slot.params.size(); ++k) {
            slot.params[k] = word(2 + k, i); // 0x616258 ... 0x5e31d8
        }
        slot.source = word(8, i); // 0x5b29c8
    }
}

std::vector<VmWake> vm_tick_sleepers(VmState& state, int ticks) {
    std::vector<VmWake> out;
    state.clock += ticks;
    // 0x457230: `remaining -= dt; if (remaining < 1) run and free the slot`,
    // walking the slots in ascending order inside the same frame.
    for (VmSleepSlot& slot : state.sleeping) {
        if (!slot.active()) {
            continue;
        }
        slot.ticks -= ticks;
        if (slot.ticks >= 1) {
            continue;
        }
        VmWake wake;
        wake.offset = static_cast<std::uint32_t>(slot.offset);
        wake.carry = -slot.ticks; // 0x457356 pushes -iVar1
        wake.source = slot.source;
        wake.params = slot.params;
        out.push_back(wake);
        slot.offset = -1; // 0x45731d: only the offset array is cleared
    }
    return out;
}

void vm_tick_haze(VmState& state, double dt_ms) {
    if (dt_ms <= 0.0 || state.haze_rate <= 0.0) {
        return;
    }
    const double dt = std::min(dt_ms, 45.0); // 0x44d518 caps the frame delta
    state.haze = std::max(0.0, state.haze - dt * state.haze_rate);
}

std::size_t vm_stop_sleeping(VmState& state, std::uint32_t handle) {
    std::size_t freed = 0;
    // 0x45b93e: `if (offset >= 0 && (source & 0xffff) == handle) offset = -1`.
    for (VmSleepSlot& slot : state.sleeping) {
        if (!slot.active() || (slot.source & 0xFFFFu) != (handle & 0xFFFFu)) {
            continue;
        }
        slot.offset = -1;
        ++freed;
    }
    return freed;
}

std::optional<int> VmState::sector_of_handle(int handle) const {
    for (const auto& [registered, sector] : sector_handles) {
        if (registered == handle) {
            return sector;
        }
    }
    return std::nullopt;
}

std::vector<int> VmState::sectors_of_handle(int handle) const {
    // 0x4583c2..0x45841c (and the twins 0x4582b3, 0x458445, 0x45849e,
    // 0x458504): the loop has no break, so every row with a matching id is
    // written, not just the first one.
    std::vector<int> out;
    for (const auto& [registered, sector] : sector_handles) {
        if (registered == handle) {
            out.push_back(sector);
        }
    }
    return out;
}

std::vector<VmPolyHandle> VmState::polys_of_handle(int handle) const {
    // 0x458566..0x4585f6 (and the twins 0x458633, 0x458701, 0x4587cd): the
    // poly handle table is walked to the end, every match is written.
    std::vector<VmPolyHandle> out;
    for (const VmPolyHandle& row : poly_handles) {
        if (row.handle == handle) {
            out.push_back(row);
        }
    }
    return out;
}

VmWorldSector* VmState::world_sector(int index) {
    if (index < 0 || index >= static_cast<int>(world_sectors.size())) {
        return nullptr;
    }
    return &world_sectors[static_cast<std::size_t>(index)];
}

const VmWorldSector* VmState::world_sector(int index) const {
    if (index < 0 || index >= static_cast<int>(world_sectors.size())) {
        return nullptr;
    }
    return &world_sectors[static_cast<std::size_t>(index)];
}

VmWorldPoly* VmState::world_poly(int sector, int object, int poly) {
    for (VmWorldPoly& record : world_polys) {
        if (record.sector == sector && record.object == object && record.poly == poly) {
            return &record;
        }
    }
    return nullptr;
}

const VmWorldPoly* VmState::world_poly(int sector, int object, int poly) const {
    for (const VmWorldPoly& record : world_polys) {
        if (record.sector == sector && record.object == object && record.poly == poly) {
            return &record;
        }
    }
    return nullptr;
}

std::size_t vm_load_sector_handles(VmState& state, std::string_view text) {
    std::size_t added = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                             : end - start);
        start = (end == std::string_view::npos) ? text.size() + 1 : end + 1;
        const std::size_t colon = line.find(':');
        const std::string_view key = line.substr(0, colon);
        if (key.find("SECTOR_HANDLE") == std::string_view::npos) {
            continue;
        }
        if (state.sector_handles.size() >= 0x100) {
            break; // 0x463820: "Maximum number of sector handles (256)"
        }
        const std::string_view rest =
            colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
        long values[2] = {0, 0};
        std::size_t cursor = 0;
        bool ok = true;
        for (long& value : values) {
            while (cursor < rest.size() && (rest[cursor] == ' ' || rest[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor >= rest.size()) {
                ok = false;
                break;
            }
            char* stop = nullptr;
            const std::string field(rest.substr(cursor));
            value = std::strtol(field.c_str(), &stop, 10);
            if (stop == field.c_str()) {
                ok = false;
                break;
            }
            cursor += static_cast<std::size_t>(stop - field.c_str());
        }
        if (!ok) {
            continue;
        }
        state.sector_handles.emplace_back(static_cast<int>(values[0]),
                                          static_cast<int>(values[1]));
        ++added;
    }
    return added;
}

std::size_t vm_load_poly_handles(VmState& state, std::string_view text) {
    // 0x463290: the reader parses "%ld %ld %ld %ld" (0x4b9640) after the
    // "POLY_HANDLE" keyword (0x4b96ac) and appends one row per line; the
    // table holds at most 0x1f4 rows ("Maximum number of poly handles (%ld)
    // has been exceeded.").
    std::size_t added = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                             : end - start);
        start = (end == std::string_view::npos) ? text.size() + 1 : end + 1;
        const std::size_t colon = line.find(':');
        const std::string_view key = line.substr(0, colon);
        if (key.find("POLY_HANDLE") == std::string_view::npos) {
            continue;
        }
        if (state.poly_handles.size() >= 0x1f4) {
            break; // 0x463295
        }
        const std::string_view rest =
            colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
        long values[4] = {0, 0, 0, 0};
        std::size_t cursor = 0;
        bool ok = true;
        for (long& value : values) {
            while (cursor < rest.size() && (rest[cursor] == ' ' || rest[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor >= rest.size()) {
                ok = false;
                break;
            }
            char* stop = nullptr;
            const std::string field(rest.substr(cursor));
            value = std::strtol(field.c_str(), &stop, 10);
            if (stop == field.c_str()) {
                ok = false;
                break;
            }
            cursor += static_cast<std::size_t>(stop - field.c_str());
        }
        if (!ok) {
            continue;
        }
        state.poly_handles.push_back(VmPolyHandle{static_cast<int>(values[0]),
                                                  static_cast<int>(values[1]),
                                                  static_cast<int>(values[2]),
                                                  static_cast<int>(values[3])});
        ++added;
    }
    return added;
}

void vm_load_world(VmState& state, const SbwLevel& level) {
    // The runtime world is the sector array 0x5670d8 (count 0x5670dc) with
    // the objects and their polygons inside.  The .sbw gives the sector type
    // (+0x14) and the polygon records; the sector light (+0x16) is zero at
    // load (0x42ae05).
    state.world_sectors.clear();
    state.world_polys.clear();
    const std::size_t sectors = level.header.sectors.size();
    state.world_sectors.reserve(sectors);
    for (std::size_t s = 0; s < sectors; ++s) {
        VmWorldSector sector;
        sector.kind = level.header.sectors[s].kind;
        state.world_sectors.push_back(sector);
    }
    // The geometry is sector-major (parse_sbw walks the sectors and, inside
    // them, the objects), so the object index inside a sector is the running
    // counter of that sector.
    std::size_t geometry_index = 0;
    for (std::size_t s = 0; s < sectors; ++s) {
        const std::size_t objects = level.header.sectors[s].objects.size();
        for (std::size_t o = 0; o < objects; ++o, ++geometry_index) {
            if (geometry_index >= level.geometry.size()) {
                return; // truncated level: stop, the rest stays empty
            }
            const SbwGeometry& geometry = level.geometry[geometry_index];
            for (std::size_t p = 0; p < geometry.polys.size(); ++p) {
                const SbwPoly& in = geometry.polys[p];
                VmWorldPoly poly;
                poly.sector = static_cast<int>(s);
                poly.object = static_cast<int>(o);
                poly.poly = static_cast<int>(p);
                poly.texture = static_cast<std::int16_t>(in.texture);
                poly.material = in.material;
                poly.scroll_u = static_cast<std::int16_t>(in.scroll_u);
                poly.scroll_v = static_cast<std::int16_t>(in.scroll_v);
                poly.brightness = in.brightness;
                state.world_polys.push_back(poly);
            }
        }
    }
}

void vm_load_sector_stuff(VmState& state, std::span<const SbtSectorSlot> slots) {
    state.sectors = {};
    const std::size_t n = std::min(slots.size(), state.sectors.size());
    for (std::size_t i = 0; i < n; ++i) {
        const SbtSectorSlot& in = slots[i];
        VmSectorSlot& out = state.sectors[i];
        out.move = in.move;
        out.extra = in.extra;
        out.sound_param = in.sound_param;
        out.event_a = in.event_a;
        out.event_b = in.event_b;
        out.damage_a = in.damage_a;
        out.damage_b = in.damage_b;
        out.sound = in.sound;
        out.sound_count = in.sound_count;
        out.flags = in.flags;
    }
}

VmScene vm_merge_scenes(const std::vector<VmPathResult>& results) {
    VmScene out;
    std::map<std::string, std::unordered_set<std::uint64_t>> seen;

    auto key = [](std::uint32_t site, int occ) {
        std::uint64_t value = static_cast<std::uint64_t>(site) << 32;
        value |= static_cast<std::uint32_t>(occ);
        return value;
    };

    auto merge = [&](const char* category, auto& target, const auto& source) {
        auto& bucket = seen[category];
        for (const auto& object : source) {
            if (bucket.insert(key(object.site, object.occ)).second) {
                target.push_back(object);
            }
        }
    };

    for (const VmPathResult& result : results) {
        const VmScene& scene = result.state.scene;
        merge("lights", out.lights, scene.lights);
        merge("locations", out.locations, scene.locations);
        merge("items", out.items, scene.items);
        merge("named_items", out.named_items, scene.named_items);
        merge("attached", out.attached, scene.attached);
        merge("sounds", out.sounds, scene.sounds);
        merge("sprites", out.sprites, scene.sprites);
        merge("things", out.things, scene.things);
        merge("doors", out.doors, scene.doors);
        merge("switches", out.switches, scene.switches);
        merge("lifts", out.lifts, scene.lifts);
        merge("spawns", out.spawns, scene.spawns);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

VmExecutor::VmExecutor(VmState state, VmOptions options)
    : initial_(std::move(state)), options_(options) {
    install_handlers();
}

void VmExecutor::install_handlers() {
    auto set = [this](std::string_view name, Handler handler) {
        if (const Def* def = find_def(kCommands, name)) {
            handlers_[def->id] = handler;
        }
    };

    set("EXIT_EVENT", &VmExecutor::h_exit_event);
    set("EXIT_PROGRAM", &VmExecutor::h_exit_program);
    set("SLEEP", &VmExecutor::h_sleep);
    set("STOP_SOURCEPLAYER_SLEEPING_SCRIPTS", &VmExecutor::h_stop_sleeping);
    set("GENERATE", &VmExecutor::h_generate);
    set("NEW_GAME", &VmExecutor::h_transition);
    set("NEW_LEVEL", &VmExecutor::h_transition);
    set("NEXT_LEVEL", &VmExecutor::h_transition);
    set("END_GAME", &VmExecutor::h_transition);
    set("ENABLE", &VmExecutor::h_enable);
    set("DISABLE", &VmExecutor::h_disable);
    set("SOURCEPLAYER_ENABLE", &VmExecutor::h_sp_enable);
    set("SOURCEPLAYER_DISABLE", &VmExecutor::h_sp_disable);
    set("OR_LEVEL_OBJECTIVES_BITS", &VmExecutor::h_or_objectives);
    set("ADD_SKY_LIGHT_SOURCE", &VmExecutor::h_sky);
    set("ADD_LIGHT_SOURCE", &VmExecutor::h_light);
    set("ADD_LOCATION", &VmExecutor::h_location);
    set("DELETE_LOCATION", &VmExecutor::h_delete_location);
    set("ADD_ITEM", &VmExecutor::h_item);
    set("ADD_ITEM_FIXED", &VmExecutor::h_item);
    set("ADD_NAMED_ITEM", &VmExecutor::h_named_item);
    set("ADD_NAMED_ITEM_EX", &VmExecutor::h_named_item_ex);
    set("ADD_SOUND_SOURCE", &VmExecutor::h_sound);
    set("ADD_SPRITE", &VmExecutor::h_sprite);
    set("ADD_DOOR", &VmExecutor::h_door);
    set("ADD_SWITCH", &VmExecutor::h_switch);
    set("SET_PLAYER_START", &VmExecutor::h_spawn);
    set("SET_DEATHMATCH_START", &VmExecutor::h_spawn);
    set("SET_CAMERA_HAZE", &VmExecutor::h_camera_haze);
    set("SET_LEVEL_NAME", &VmExecutor::h_level);
    set("SET_LEVEL_GRAVITY", &VmExecutor::h_level);
    set("SET_LEVEL_FOG_COLOUR", &VmExecutor::h_level);
    set("SET_LEVEL_FOG_DISTANCE", &VmExecutor::h_level);
    set("SET_LEVEL_COUNTDOWN_TIMER", &VmExecutor::h_level);
    set("SET_LEVEL_FOCAL_ADJUST", &VmExecutor::h_level);
    set("SET_LEVEL_ALIASING_VALUES", &VmExecutor::h_level);
    set("SET_LEVEL_VERSION", &VmExecutor::h_level);
    set("SET_LEVEL_BEST_TIME", &VmExecutor::h_level);
    set("SET_LEVEL_SECRETS", &VmExecutor::h_level);
    set("SET_LEVEL_END_CAMERA_XYZ", &VmExecutor::h_level);
    // Music and overlay text: both used to fall through to the generic event
    // log; they keep that event and additionally record the state (M5.2.5).
    for (const char* name : {"LOAD_MUSIC", "PLAY_MUSIC", "STOP_MUSIC",
                             "UNLOAD_MUSIC", "MEMORIZE_CURRENT_MUSIC_BANK",
                             "PLAY_MEMORIZED_MUSIC_BANK"}) {
        set(name, &VmExecutor::h_music);
    }
    set("ADD_SCREEN_TEXT", &VmExecutor::h_screen_text);
    set("ADD_SOURCESCREEN_TEXT", &VmExecutor::h_screen_text);
    // Screen requests keep the event and record which screen the script asked
    // for; the frontend is what actually switches (M8.2/M9.5).
    for (const char* name : {"SHOW_LEVEL_END_SCREEN", "REFRESH_SCREEN",
                             "SHOW_AMMO_BUY_SCREEN", "SHOW_ITEM_BUY_SCREEN",
                             "SHOW_TANK_BUY_SCREEN", "SHOW_TANK_SAVE_SCREEN",
                             "SHOW_WEAPON_BUY_SCREEN", "SHOW_BRIEFING_SCREEN",
                             "SHOW_HELP_SCREEN"}) {
        set(name, &VmExecutor::h_screen);
    }
    set("ADD_SOURCEPLAYER_AMMO", &VmExecutor::h_add_ammo);
    set("ADD_SOURCEPLAYER_CASH", &VmExecutor::h_cash);
    set("HEAL_SOURCEPLAYER", &VmExecutor::h_heal);
    set("REPAIR_SOURCEPLAYER", &VmExecutor::h_heal);
    set("SOURCEPLAYER_EQUIPMENT_ACTIVATE", &VmExecutor::h_equip);
    set("SOURCEPLAYER_EQUIPMENT_INDEX", &VmExecutor::h_index);
    set("SOURCEPLAYER_WEAPON_INDEX", &VmExecutor::h_index);
    set("SET_SOURCEPLAYER_TYPE", &VmExecutor::h_playertype);
    set("SOURCEPLAYER_MOVEMENT_TYPE", &VmExecutor::h_movement);
    set("ADD_SOURCEPLAYER_SALVAGE", &VmExecutor::h_salvage);
    set("ADJUST_SOURCEPLAYER_MORALE", &VmExecutor::h_morale);
    set("INCREMENT_SOURCEPLAYER_SECRET_TALLY", &VmExecutor::h_secret);
    set("DEFINE_SHIELDS", &VmExecutor::h_def);
    set("DEFINE_VIRUS", &VmExecutor::h_def);
    set("DEFINE_REGENERATION_MARKER", &VmExecutor::h_def);
    set("SOURCE_CLEAR", &VmExecutor::h_source_clear);
    // SECTOR_STUFF writers: the five commands that touch 0x800b60.  They keep
    // their "event" classification (vm_command_status), only the handler
    // differs from the plain event log.
    set("SET_SECTOR_MOVEMENT", &VmExecutor::h_sector);
    set("SET_SECTOR_DAMAGE", &VmExecutor::h_sector);
    set("SET_SECTOR_SOUND", &VmExecutor::h_sector);
    set("SET_SECTOR_EVENTS", &VmExecutor::h_sector);
    set("SET_SECTOR_FLAGS", &VmExecutor::h_sector);
    // Same array addressed through the implicit source sector (edi) or a
    // sector handle (0x5b1df8/0x617fb8).
    for (const char* name : {"SET_SOURCESECTOR_MOVEMENT", "SET_SOURCESECTOR_FLAGS",
                             "SET_SOURCESECTOR_DAMAGE", "SET_HANDLESECTOR_MOVEMENT",
                             "SET_HANDLESECTOR_FLAGS", "SET_HANDLESECTOR_DAMAGE",
                             "SET_HANDLESECTOR_SOUND"}) {
        const auto info = vm_command_info(name);
        if (info.has_value()) {
            handlers_[info->op] = &VmExecutor::h_sector;
        }
    }
    // The world sector (0x5670d8) and the world polygons (the 0x80-byte
    // records): brightness, type, texture, material and scroll are real
    // state, not events.
    for (const char* name : {"SET_SECTOR_BRIGHTNESS", "SET_SECTOR_TYPE",
                             "SET_SOURCESECTOR_BRIGHTNESS", "SET_SOURCESECTOR_TYPE",
                             "SET_HANDLESECTOR_BRIGHTNESS", "SET_HANDLESECTOR_TYPE"}) {
        const auto info = vm_command_info(name);
        if (info.has_value()) {
            handlers_[info->op] = &VmExecutor::h_world_sector;
        }
    }
    for (const char* name : {"SET_POLY_TEXTURE", "SET_POLY_BRIGHTNESS", "SET_POLY_SCROLL",
                             "SET_SOURCEPOLY_TEXTURE", "SET_SOURCEPOLY_BRIGHTNESS",
                             "SET_SOURCEPOLY_MATERIAL", "SET_SOURCEPOLY_SCROLL",
                             "SET_HANDLEPOLY_TEXTURE", "SET_HANDLEPOLY_BRIGHTNESS",
                             "SET_HANDLEPOLY_SCROLL", "SET_HANDLEPOLY_MATERIAL"}) {
        const auto info = vm_command_info(name);
        if (info.has_value()) {
            handlers_[info->op] = &VmExecutor::h_world_poly;
        }
    }
    set("SOURCEPLAYER_KILL", &VmExecutor::h_kill);

    for (const auto& def : kCommands) {
        const std::string_view name = def.name;
        if (contains(kState, name) && name.find("BITS") != std::string_view::npos) {
            handlers_.emplace(def.id, &VmExecutor::h_bits);
        }
        const VmCommandStatus status = vm_command_status(def.id);
        if (status == VmCommandStatus::Attached) {
            handlers_.emplace(def.id, &VmExecutor::h_attached);
        } else if (status == VmCommandStatus::Event) {
            handlers_.emplace(def.id, &VmExecutor::h_event);
        }
    }
}

bool VmExecutor::expired() const {
    return deadline_.has_value() && std::chrono::steady_clock::now() > *deadline_;
}

void VmExecutor::note(std::string message) {
    if (std::find(notes_.begin(), notes_.end(), message) == notes_.end()) {
        notes_.push_back(std::move(message));
    }
}

void VmExecutor::trace(VmState& state, const VmInsn& insn, const std::string& note) {
    if (!options_.trace || state.trace.size() >= options_.trace_limit) {
        return;
    }
    VmTraceEvent event;
    event.pc = insn.pc;
    event.depth = static_cast<int>(state.stack.size());
    event.name = insn.name;
    event.args = insn.args;
    event.note = note;
    state.trace.push_back(std::move(event));
}

void VmExecutor::event(VmState& state, const VmInsn& insn, const std::string& note) {
    if (state.events.size() < kMaxEvents) {
        state.events.push_back(VmEvent{insn.pc, insn.name, insn.args, note});
    }
}

std::vector<VmPathResult> VmExecutor::run(std::size_t start,
                                         std::optional<int> source_entity) {
    if (options_.max_time > 0.0) {
        deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(options_.max_time));
    }

    VmState initial = initial_;
    initial.pc = start;
    if (source_entity) {
        initial.source_entity = source_entity;
    }

    Frontier frontier;
    frontier.emplace_back(std::move(initial), start);
    std::vector<VmPathResult> results;

    while (!frontier.empty()) {
        auto entry = std::move(frontier.front());
        frontier.pop_front();
        VmState state = std::move(entry.first);
        const std::size_t pc = entry.second;

        if (expired()) {
            VmPathResult result;
            result.reason = VmReason::Deadline;
            result.pc = state.pc;
            result.detail = "wall clock";
            result.steps = state.steps;
            result.state = std::move(state);
            results.push_back(std::move(result));
            continue;
        }

        VmPathResult result;
        try {
            const Stop stop = run_path(state, pc, frontier);
            result.reason = stop.reason;
            result.pc = state.pc;
            result.detail = stop.detail;
        } catch (const HaltSignal& halt) {
            state.pc = halt.pc;
            result.reason = halt.reason;
            result.pc = halt.pc;
            result.detail = halt.detail;
        } catch (const VmBadOperand& bad) {
            result.reason = VmReason::BadOperand;
            result.pc = bad.pc;
            result.detail = bad.detail;
        } catch (const VmBadPC& bad) {
            result.reason = VmReason::BadPC;
            result.pc = bad.pc;
            result.detail = "pc outside code";
        } catch (const std::exception& error) {
            // Defensive: a malformed operand list must not escape as an
            // uncaught C++ exception (Python would report a bad operand).
            result.reason = VmReason::BadOperand;
            result.pc = state.pc;
            result.detail = error.what();
        }
        result.steps = state.steps;
        result.state = std::move(state);
        results.push_back(std::move(result));
    }
    return results;
}

VmExecutor::Stop VmExecutor::run_path(VmState& state, std::size_t pc, Frontier& frontier) {
    while (true) {
        ++state.steps;
        if (state.steps > options_.max_steps) {
            return Stop{VmReason::StepLimit, pc,
                        std::to_string(state.steps) + " steps"};
        }
        if (expired()) {
            return Stop{VmReason::Deadline, pc, "wall clock"};
        }

        VmInsn insn = decode_insn(state.code, pc);
        state.pc = pc;
        trace(state, insn);

        switch (insn.kind) {
        case VmInsnKind::End: {
            const auto next = leave_frame(state);
            if (!next) {
                return Stop{VmReason::End, pc, ""};
            }
            pc = *next;
            continue;
        }

        case VmInsnKind::If:
            pc = do_if(state, insn, frontier);
            continue;

        case VmInsnKind::Else:
            if (insn.target > state.code.size()) {
                throw HaltSignal{VmReason::BadPC, insn.pc,
                                 "ELSE target " + hexs(insn.target)};
            }
            pc = insn.target;
            continue;

        case VmInsnKind::Eval:
            (void)eval(state, insn, false);
            pc = insn.next;
            continue;

        case VmInsnKind::Unknown:
            return Stop{VmReason::UnknownOpcode, pc,
                        "opcode " + hexs8(insn.op) + " at " + hexs(insn.pc)};

        case VmInsnKind::Table:
        case VmInsnKind::Objectives:
        case VmInsnKind::Path:
        case VmInsnKind::Lift:
            structural(state, insn);
            pc = insn.next;
            continue;

        case VmInsnKind::Cmd:
            break;
        }

        ++command_counts_[insn.op];
        const VmCommandStatus status = vm_command_status(insn.op);
        if (status == VmCommandStatus::Unsupported) {
            return Stop{VmReason::UnsupportedCommand, pc,
                        insn.name + " (op " + std::to_string(insn.op) + ")"};
        }
        const auto handler = handlers_.find(insn.op);
        if (handler == handlers_.end()) {
            return Stop{VmReason::UnsupportedCommand, pc,
                        insn.name + " (op " + std::to_string(insn.op) +
                            ") has no handler"};
        }
        const auto next = (this->*(handler->second))(state, insn);
        pc = next.has_value() ? *next : insn.next;
        if (pc > state.code.size()) {
            throw HaltSignal{VmReason::BadPC, pc, ""};
        }
    }
}

std::size_t VmExecutor::do_if(VmState& state, const VmInsn& insn, Frontier& frontier) {
    const std::optional<bool> value = eval(state, insn, true);
    if (value.has_value()) {
        return *value ? insn.body_start : insn.body_end;
    }

    switch (options_.on_unknown) {
    case VmUnknownPolicy::Stop:
        throw HaltSignal{VmReason::UnknownCondition, insn.pc,
                         insn.condition.empty() ? "?" : insn.condition};
    case VmUnknownPolicy::False:
        note("IF " + insn.condition + " assumed false at " + hexs(insn.pc));
        return insn.body_end;
    case VmUnknownPolicy::True:
        note("IF " + insn.condition + " assumed true at " + hexs(insn.pc));
        return insn.body_start;
    case VmUnknownPolicy::Both:
        break;
    }

    // Both: explore the else path from a clone, continue with the then path.
    if (static_cast<int>(frontier.size()) + 1 >= options_.max_paths) {
        note("IF " + insn.condition + " fork capped at " + hexs(insn.pc) +
             " (took false)");
        return insn.body_end;
    }
    VmState alternate = state;
    alternate.pc = insn.body_end;
    trace(alternate, insn, "fork: else");
    frontier.emplace_back(std::move(alternate), insn.body_end);
    trace(state, insn, "fork: then");
    return insn.body_start;
}

std::optional<bool> VmExecutor::eval(VmState& state, const VmInsn& insn, bool as_if) {
    const int cid = cond_id(insn.condition);
    ++condition_counts_[insn.condition];
    const auto& args = insn.cond_args;

    if (kActionConditions.find(cid) != kActionConditions.end()) {
        action(state, cid, insn.condition, args, insn);
        if (cid == 1 || cid == 2 || cid == 3 || cid == 4 || cid == 5 || cid == 27) {
            return true;
        }
        return std::nullopt;
    }

    if (kKnownConditions.find(cid) == kKnownConditions.end()) {
        note("unknown condition " + insn.condition + " at " + hexs(insn.pc));
        if (!as_if) {
            throw HaltSignal{VmReason::UnknownCondition, insn.pc, insn.condition};
        }
        return std::nullopt;
    }

    try {
        return check(state, cid, args);
    } catch (const std::out_of_range&) {
        note("cannot evaluate " + insn.condition + " at " + hexs(insn.pc) +
             ": missing operand");
        if (as_if) {
            return std::nullopt;
        }
        throw HaltSignal{VmReason::UnknownCondition, insn.pc, insn.condition};
    } catch (const VmBadOperand& bad) {
        note("cannot evaluate " + insn.condition + " at " + hexs(insn.pc) +
             ": " + bad.detail);
        if (as_if) {
            return std::nullopt;
        }
        throw HaltSignal{VmReason::UnknownCondition, insn.pc, insn.condition};
    }
}

std::optional<bool> VmExecutor::check(VmState& state, int cid,
                                      const std::vector<SbtValue>& args) {
    VmPlayer* player = state.player();
    switch (cid) {
    case 6: // GAME_DIFFICULTY_AT_LEAST
        return state.difficulty >= arg_i32(args.at(0));
    case 7: { // LEVEL_OBJECTIVES_COMPLETE
        if (state.objectives.empty()) {
            return false;
        }
        const std::size_t n = state.objectives.size();
        const std::uint32_t mask = (1u << n) - 1u;
        return (state.objectives_bits & mask) == mask;
    }
    case 8: // NEXT_LEVEL_NUMBER (0x45ea26: ds:0x80f9cc == n)
        return state.mission_counter == arg_i32(args.at(0));
    case 9: // SOURCE_IS_PLAYER
        return state.source_player.has_value();
    case 10: // SOURCE_IS_LOCAL_PLAYER_ONE
        return state.source_player.has_value() && *state.source_player == 0;
    case 11: // SOURCE_IS_LOCAL_PLAYER_TWO
        return state.source_player.has_value() && *state.source_player == 1;
    case 12: // DEATHMATCH_GAME
        return state.game_mode == 1 || state.game_mode == 2;
    case 13: // DEATHMATCH_CLASSIC_GAME
        return state.game_mode == 2;
    case 14: // DEATHMATCH_NORMAL_GAME
        return state.game_mode == 1;
    case 15: // MULTIPLAYER_GAME
        return state.game_mode != 0 || state.player_count > 1;
    case 16: // CAPTURE_THE_FLAG_GAME
        return (state.mp_flags & 2) != 0;
    case 17: // TEAM_GAME
        return (state.mp_flags & 1) != 0;
    case 23: // PLAYERS_AT_LEAST
        return state.player_count >= arg_i32(args.at(0));
    case 24: // RANDOM
    case 25: // RANDOM_FRAME_BALANCED
        if (options_.random_unknown) {
            return std::nullopt;
        }
        return state.rnd() < arg_f64(args.at(0));
    case 26: // GOD_MODE
        return state.god_mode;
    case 28: // SCAVENGERHUNT_GAME
        // 0x45e26b -> 0x4421d0: `cmp ecx,3 / sete` (docs/DM_RULES.md §3).
        return state.game_mode == 3;
    default:
        break;
    }

    // Every SOURCEPLAYER_* check below reads the current source player; with no
    // source (a level-level script) the value is genuinely open.
    if (player == nullptr) {
        return std::nullopt;
    }
    switch (cid) {
    case 30: // SOURCEPLAYER_KEY_BITS_SET
        return (player->key_bits & arg_u32(args.at(0))) != 0;
    case 31: // SOURCEPLAYER_WEAPON_BITS_SET
        return (player->weapon_bits & arg_u32(args.at(0))) != 0;
    case 33: { // SOURCEPLAYER_AMMO_AT_LEAST
        const int type = arg_i32(args.at(0));
        const auto it = player->ammo.find(type);
        const int have = it == player->ammo.end() ? 0 : it->second;
        return have >= arg_i32(args.at(1));
    }
    case 34: // SOURCEPLAYER_CASH_AT_LEAST
        return player->cash >= arg_i32(args.at(0));
    case 36: // SOURCEPLAYER_IS
        return state.source_player.has_value() &&
               *state.source_player == arg_i32(args.at(0));
    case 37: // SOURCEPLAYER_SHIELD_FULL
        return player->shield >= player->max_shield;
    case 38: // SOURCEPLAYER_LIFE_AT_LEAST
        return player->health >= arg_f64(args.at(0));
    case 39: // SOURCEPLAYER_LIFE_FULL
        return player->health >= player->max_health;
    case 43: // SOURCEPLAYER_TYPE
        return player->playertype == arg_i32(args.at(0));
    default:
        return std::nullopt;
    }
}

void VmExecutor::action(VmState& state, int cid, const std::string& name,
                        const std::vector<SbtValue>& args, const VmInsn& insn) {
    if (cid >= 1 && cid <= 5) {
        thing(state, name, args, insn);
        event(state, insn, name);
        return;
    }
    if (cid == 27) { // SOURCE_SWAP
        std::swap(state.source_entity, state.source_alt);
        event(state, insn, "source swapped");
        return;
    }
    if (cid == 18 || cid == 19 || cid == 20) { // FIRE_*
        event(state, insn, "projectile spawn not simulated");
        return;
    }
    if (cid == 41) {
        event(state, insn, "handle teleport");
        return;
    }
    if (cid == 42) {
        event(state, insn, "player teleport");
        return;
    }
    if (cid == 45) {
        event(state, insn, "player warp");
        return;
    }
    if (cid == 46 || (cid >= 52 && cid <= 59)) {
        event(state, insn, "custom/engine action");
        return;
    }
    event(state, insn);
}

void VmExecutor::thing(VmState& state, const std::string& name,
                       const std::vector<SbtValue>& args, const VmInsn& insn) {
    const std::string_view sig = condition_signature(name);
    std::optional<int> type_index;
    int owner = 0;
    std::vector<int> ints;
    std::vector<double> doubles;

    for (std::size_t k = 0; k < sig.size() && k < args.size(); ++k) {
        const char letter = sig[k];
        const SbtValue& value = args[k];
        if (letter == 'G') {
            type_index = arg_i32(value);
        } else if (letter == 'A' || letter == 'B' || letter == 'D') {
            doubles.push_back(arg_f64(value));
        } else if (value.kind != SbtValue::Kind::String) {
            if (!type_index.has_value() && ints.empty()) {
                owner = arg_i32(value);
            } else {
                ints.push_back(arg_i32(value));
            }
        }
    }

    if (!type_index.has_value()) {
        event(state, insn, "thing without a type operand");
        return;
    }

    VmThing thing;
    thing.entity = state.source_entity;
    thing.kind = name;
    thing.type_index = *type_index;
    const bool absolute = name == "ADD_THING" || name == "ADD_THING_EX";
    if (absolute && ints.size() >= 3) {
        thing.pos = std::array<int, 3>{ints[0], ints[1], ints[2]};
    }
    thing.angles = std::move(doubles);
    thing.owner = owner;
    add(state, insn, state.scene.things, std::move(thing));
}

void VmExecutor::structural(VmState& state, const VmInsn& insn) {
    for (const auto& table : kTables) {
        if (insn.name == table.name) {
            state.table_defs[insn.name] = insn.table;
            if (insn.name == "DEFINE_AMMO") {
                // Handler 0x459d14: ammo[type] is clamped to the record's second
                // value (array 0x818960, stride 0x38, +4).
                for (std::size_t n = 0; n < insn.table.size(); ++n) {
                    const auto& record = insn.table[n];
                    if (record.values.size() > 1) {
                        for (auto& [index, player] : state.players) {
                            (void)index;
                            player.ammo_max[static_cast<int>(n)] =
                                arg_i32(record.values[1]);
                        }
                    }
                }
            }
            return;
        }
    }
    if (insn.name == "DEFINE_LEVEL_OBJECTIVES") {
        state.objectives.clear();
        for (const auto& value : insn.args) {
            state.objectives.push_back(value.kind == SbtValue::Kind::String ? value.s
                                                                           : value_text(value));
        }
        return;
    }
    if (insn.name == "SOURCEPLAYER_PATH") {
        state.path = insn.ints;
        return;
    }
    if (insn.name == "ADD_LIFT") {
        VmLift lift;
        lift.head = insn.lift_head;
        lift.stops = insn.stops;
        add(state, insn, state.scene.lifts, std::move(lift));
    }
}

// -- flow ------------------------------------------------------------------

std::optional<std::size_t> VmExecutor::h_exit_event(VmState& state, const VmInsn& insn) {
    const auto next = leave_frame(state);
    if (!next) {
        throw HaltSignal{VmReason::End, insn.pc, "EXIT_EVENT at depth 0"};
    }
    return *next;
}

std::optional<std::size_t> VmExecutor::h_exit_program(VmState& /*state*/,
                                                     const VmInsn& insn) {
    throw HaltSignal{VmReason::ProgramExit, insn.pc, "EXIT_PROGRAM"};
}

std::optional<std::size_t> VmExecutor::h_sleep(VmState& state, const VmInsn& insn) {
    const int value = arg_i32(insn.args.at(0));
    if (!options_.sleep_queue) {
        // Exploration shortcut (Python reference): keep the remainder inline.
        state.clock += value;
        event(state, insn, "sleep " + std::to_string(value));
        return std::nullopt;
    }

    // 0x457390 case 7 (0x45c43b): `delay = operand - carry`.  A non-positive
    // delay is consumed at once and the leftover becomes the new carry
    // (0x45c4b8: `param_2 = -iVar19`); a positive one suspends the script.
    const int delay = value - state.sleep_carry;
    if (delay <= 0) {
        state.sleep_carry = -delay;
        event(state, insn, "sleep " + std::to_string(value) + " (elapsed)");
        return std::nullopt;
    }

    state.sleep_carry = 0;
    for (VmSleepSlot& slot : state.sleeping) {
        if (slot.active()) {
            continue;
        }
        slot.offset = static_cast<std::int32_t>(insn.next);
        slot.ticks = delay;
        slot.params = {};
        slot.source = source_handle(state);
        event(state, insn,
              "sleep " + std::to_string(value) + " ticks=" + std::to_string(delay) +
                  " at " + hexs(insn.next));
        // 0x45c4ba: return from 0x457390 — the remainder of this script (and
        // of the caller's, since the engine called us) continues later.
        const auto next = leave_frame(state);
        if (!next) {
            throw HaltSignal{VmReason::End, insn.pc, "SLEEP at depth 0"};
        }
        return *next;
    }
    // "Maximum sleeping scripts (256 reached)" (0x4b8134): the engine prints
    // it and returns from 0x457390 — the rest of this script is dropped, the
    // caller (if any) continues.
    note("maximum sleeping scripts (256 reached)");
    event(state, insn, "maximum sleeping scripts (256 reached)");
    const auto next = leave_frame(state);
    if (!next) {
        throw HaltSignal{VmReason::End, insn.pc, "maximum sleeping scripts"};
    }
    return *next;
}

std::optional<std::size_t> VmExecutor::h_stop_sleeping(VmState& state, const VmInsn& insn) {
    const std::size_t freed = vm_stop_sleeping(state, source_handle(state));
    event(state, insn, freed == 0 ? "" : "stopped " + std::to_string(freed) + " sleeping");
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_transition(VmState& state,
                                                    const VmInsn& insn) {
    // `NEXT_LEVEL` with no operand is the campaign step: the target name is
    // built from the counter *before* the increment (0x457b17..0x457b2f).
    if (insn.name == "NEXT_LEVEL" && insn.args.empty()) {
        const std::string detail =
            "NEXT_LEVEL LEVEL" + std::to_string(state.mission_counter);
        ++state.mission_counter;
        throw HaltSignal{VmReason::LevelTransition, insn.pc, detail};
    }
    throw HaltSignal{VmReason::LevelTransition, insn.pc,
                     insn.name + " " + args_text(insn.args)};
}

std::optional<std::uint32_t> VmExecutor::next_generate_row(VmState& state, int gid,
                                                          std::size_t& cursor) {
    // Which flag bit the source needs (0x45fa74..0x45fad2).  Without a source
    // the model cannot pick one bit, so it takes the union of all sources
    // (`flags & 0x1ff`), i.e. runs any row that is enabled at all — a fired
    // one-shot (0x200 alone) and a DISABLED row (0) stay out, like the engine.
    const std::optional<std::uint32_t> bit = state.source_player
                                                 ? std::optional<std::uint32_t>(
                                                       source_player_bit(state))
                                                 : std::nullopt;
    for (std::size_t i = cursor; i < state.entities.size(); ++i) {
        VmEntity& row = state.entities[i];
        if (row.id != gid) {
            continue;
        }
        if (bit ? (row.flags & *bit) == 0 : !row.enabled()) {
            continue;
        }
        // 0x45fa9e / 0x45fad9: the one-shot bit clears 0x1ff *before* the
        // sub-script runs (0x45fac6 `JMP` into the call), so the run sees the
        // row disabled and a self-GENERATE cannot re-enter it.
        if (row.one_shot()) {
            row.flags &= 0xFFFFFE00u;
        }
        cursor = i + 1;
        return row.offset;
    }
    cursor = state.entities.size();
    return std::nullopt;
}

std::size_t VmExecutor::enter_generate(VmState& state, int gid, std::uint32_t offset,
                                       std::size_t cursor, const VmInsn& insn) {
    if (offset >= state.code.size()) {
        throw HaltSignal{VmReason::BadPC, insn.pc, "entity offset " + hexs(offset)};
    }
    if (std::find(state.active.begin(), state.active.end(), offset) !=
        state.active.end()) {
        throw HaltSignal{VmReason::Recursion, insn.pc,
                         "GENERATE " + std::to_string(gid) + " re-enters offset " +
                             hexs(offset)};
    }
    if (state.stack.size() >= kMaxDepth) {
        throw HaltSignal{VmReason::StackOverflow, insn.pc,
                         "depth " + std::to_string(state.stack.size())};
    }
    state.stack.push_back(VmStackFrame{insn.next, state.source_entity, gid, cursor,
                                       static_cast<std::uint32_t>(insn.pc), state.sleep_carry});
    state.active.push_back(offset);
    state.source_entity = gid;
    state.sleep_carry = 0; // 0x45fac3 pushes 0 as 0x457390's second argument
    return offset;
}

std::optional<std::size_t> VmExecutor::leave_frame(VmState& state) {
    if (state.stack.empty()) {
        return std::nullopt;
    }
    VmStackFrame frame = state.stack.back();
    state.stack.pop_back();
    if (!state.active.empty()) {
        state.active.pop_back();
    }
    state.source_entity = frame.source;
    state.sleep_carry = frame.carry;
    if (frame.generate_id) {
        std::size_t cursor = frame.row_cursor;
        const auto offset = next_generate_row(state, *frame.generate_id, cursor);
        if (offset) {
            // The remaining rows of the same id run one by one (0x45fb1c
            // loops back to 0x45fa58 after the call returns).
            if (*offset >= state.code.size()) {
                throw HaltSignal{VmReason::BadPC, frame.site,
                                 "entity offset " + hexs(*offset)};
            }
            if (std::find(state.active.begin(), state.active.end(), *offset) !=
                state.active.end()) {
                throw HaltSignal{VmReason::Recursion, frame.site,
                                 "GENERATE " + std::to_string(*frame.generate_id) +
                                     " re-enters offset " + hexs(*offset)};
            }
            if (state.stack.size() >= kMaxDepth) {
                throw HaltSignal{VmReason::StackOverflow, frame.site,
                                 "depth " + std::to_string(state.stack.size())};
            }
            state.stack.push_back(VmStackFrame{frame.return_pc, frame.source,
                                               frame.generate_id, cursor, frame.site, 0});
            state.active.push_back(*offset);
            state.source_entity = *frame.generate_id;
            state.sleep_carry = 0;
            return *offset;
        }
    }
    return frame.return_pc;
}

std::optional<std::size_t> VmExecutor::h_generate(VmState& state, const VmInsn& insn) {
    const int gid = arg_i32(insn.args.at(0));
    for (auto& door : state.scene.doors) {
        for (const int id : door.ids) {
            if (id == gid) {
                ++door.toggles;
            }
        }
    }

    // 0x45fa00 counts every matching row (local_4) before the flag tests; the
    // "event not defined" warning at 0x45fb2e only fires when there is none.
    const bool known = state.entity(gid) != nullptr;
    event(state, insn, "generate id=" + std::to_string(gid) + (known ? "" : " (no entity)"));

    if (!options_.follow_generate) {
        return std::nullopt;
    }
    std::size_t cursor = 0;
    const auto offset = next_generate_row(state, gid, cursor);
    if (!offset) {
        return std::nullopt;
    }
    return enter_generate(state, gid, *offset, cursor, insn);
}

// -- entity flags ----------------------------------------------------------

std::optional<std::size_t> VmExecutor::h_enable(VmState& state, const VmInsn& insn) {
    // 0x457c67 walks the whole id column, no break: every match changes.
    const int id = arg_i32(insn.args.at(0));
    for (VmEntity& row : state.entities) {
        if (row.id == id) {
            row.flags |= 0x1FFu;
        }
    }
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_disable(VmState& state, const VmInsn& insn) {
    const int id = arg_i32(insn.args.at(0));
    for (VmEntity& row : state.entities) {
        if (row.id == id) {
            row.flags &= ~0x1FFu;
        }
    }
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_sp_enable(VmState& state, const VmInsn& insn) {
    const int id = arg_i32(insn.args.at(0));
    for (VmEntity& row : state.entities) {
        if (row.id == id) {
            row.flags |= source_player_bit(state);
        }
    }
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_sp_disable(VmState& state, const VmInsn& insn) {
    const int id = arg_i32(insn.args.at(0));
    for (VmEntity& row : state.entities) {
        if (row.id == id) {
            row.flags &= ~source_player_bit(state);
        }
    }
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_or_objectives(VmState& state, const VmInsn& insn) {
    state.objectives_bits |= arg_u32(insn.args.at(0));
    return std::nullopt;
}

// -- scene ----------------------------------------------------------------

std::optional<std::size_t> VmExecutor::h_sky(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmLight light;
    light.kind = "sky";
    light.pos = {arg_i32(args.at(0)), arg_i32(args.at(1)), arg_i32(args.at(2))};
    light.param = args.size() > 3 ? arg_i32(args.at(3)) : 0;
    if (args.size() >= 7) {
        light.rgb = {channel(args.at(4)), channel(args.at(5)), channel(args.at(6))};
    }
    add(state, insn, state.scene.lights, std::move(light));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_light(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmLight light;
    light.kind = "local";
    light.pos = {arg_i32(args.at(0)), arg_i32(args.at(1)), arg_i32(args.at(2))};
    light.param = args.size() > 3 ? arg_i32(args.at(3)) : 0;
    if (args.size() >= 7) {
        light.rgb = {channel(args.at(4)), channel(args.at(5)), channel(args.at(6))};
    }
    if (args.size() >= 12) {
        for (std::size_t k = 9; k < 12; ++k) {
            light.extra.push_back(arg_i32(args.at(k)));
        }
    }
    add(state, insn, state.scene.lights, std::move(light));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_location(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmLocation location;
    location.thing_id = arg_i32(args.at(0));
    location.pos = {arg_i32(args.at(1)), arg_i32(args.at(2)), arg_i32(args.at(3))};
    for (std::size_t k = 0; k < 3; ++k) {
        location.orientation[k] = arg_f64(args.at(4 + k));
    }
    add(state, insn, state.scene.locations, std::move(location));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_delete_location(VmState& state,
                                                         const VmInsn& insn) {
    const int id = arg_i32(insn.args.at(0));
    auto& locations = state.scene.locations;
    locations.erase(std::remove_if(locations.begin(), locations.end(),
                                   [id](const VmLocation& location) {
                                       return location.thing_id == id;
                                   }),
                    locations.end());
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_item(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmItem item;
    item.id = arg_i32(args.at(1));
    item.pos = {arg_i32(args.at(5)), arg_i32(args.at(6)), arg_i32(args.at(7))};
    item.fixed = insn.name == "ADD_ITEM_FIXED";
    item.named = false;
    for (std::size_t k = 0; k < 5; ++k) {
        item.extra.push_back(SbtValue::from_uint(static_cast<std::uint32_t>(
            arg_i32(args.at(k)))));
    }
    add(state, insn, state.scene.items, std::move(item));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_named_item(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmItem item;
    item.id = arg_i32(args.at(0));
    item.pos = {arg_i32(args.at(1)), arg_i32(args.at(2)), arg_i32(args.at(3))};
    item.fixed = false;
    item.named = true;
    for (std::size_t k = 4; k < 7; ++k) {
        item.extra.push_back(args.at(k));
    }
    add(state, insn, state.scene.named_items, std::move(item));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_named_item_ex(VmState& state,
                                                       const VmInsn& insn) {
    const auto& args = insn.args;
    if (args.size() < 10) {
        throw HaltSignal{VmReason::BadOperand, insn.pc, "ADD_NAMED_ITEM_EX"};
    }
    VmItem item;
    item.id = arg_i32(args.at(3));
    item.pos = {arg_i32(args.at(4)), arg_i32(args.at(5)), arg_i32(args.at(6))};
    item.fixed = false;
    item.named = true;
    for (std::size_t k = 7; k < 10; ++k) {
        item.extra.push_back(args.at(k));
    }
    add(state, insn, state.scene.named_items, std::move(item));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_sound(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmSound sound;
    sound.pos = {arg_i32(args.at(0)), arg_i32(args.at(1)), arg_i32(args.at(2))};
    sound.sample = args.size() > 3 ? arg_i32(args.at(3)) : 0;
    sound.volume = args.size() > 4 ? arg_i32(args.at(4)) : 0;
    sound.pitch = args.size() > 5 ? arg_f64(args.at(5)) : 0.0;
    add(state, insn, state.scene.sounds, std::move(sound));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_sprite(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmSprite sprite;
    sprite.pos = {arg_f64(args.at(1)), arg_f64(args.at(2)), arg_f64(args.at(3))};
    sprite.jitter = arg_i32(args.at(8));
    sprite.id = arg_i32(args.at(0));
    add(state, insn, state.scene.sprites, std::move(sprite));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_door(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmDoor door;
    door.flags = arg_i32(args.at(0));
    for (std::size_t k = 0; k < 5; ++k) {
        door.ids[k] = arg_i32(args.at(1 + k));
    }
    for (std::size_t k = 0; k < 3; ++k) {
        door.angles[k] = arg_f64(args.at(6 + k));
    }
    for (std::size_t k = 0; k < 7; ++k) {
        door.params[k] = arg_i32(args.at(9 + k));
    }
    for (std::size_t k = 0; k < 9; ++k) {
        door.values[k] = arg_f64(args.at(16 + k));
    }
    for (std::size_t k = 0; k < 6; ++k) {
        door.tail[k] = arg_i32(args.at(25 + k));
    }
    add(state, insn, state.scene.doors, std::move(door));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_switch(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    VmSwitch sw;
    sw.kind = arg_i32(args.at(0));
    sw.id = arg_i32(args.at(1));
    sw.radius = arg_i32(args.at(2));
    sw.pos = {arg_i32(args.at(3)), arg_i32(args.at(4)), arg_i32(args.at(5))};
    sw.extra = {arg_i32(args.at(6)), arg_i32(args.at(7))};
    add(state, insn, state.scene.switches, std::move(sw));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_spawn(VmState& state, const VmInsn& insn) {
    const auto& args = insn.args;
    if (args.size() < 7) {
        throw HaltSignal{VmReason::BadOperand, insn.pc, insn.name};
    }
    VmSpawn spawn;
    spawn.index = arg_i32(args.at(0));
    spawn.pos = {arg_i32(args.at(1)), arg_i32(args.at(2)), arg_i32(args.at(3))};
    for (std::size_t k = 0; k < 3; ++k) {
        spawn.angles[k] = arg_f64(args.at(4 + k));
    }
    spawn.kind = insn.name == "SET_PLAYER_START" ? "campaign" : "deathmatch";
    add(state, insn, state.scene.spawns, std::move(spawn));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_attached(VmState& state, const VmInsn& insn) {
    VmAttached attached;
    attached.kind = insn.name;
    attached.args = insn.args;
    attached.source = state.source_entity;
    add(state, insn, state.scene.attached, std::move(attached));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_event(VmState& state, const VmInsn& insn) {
    event(state, insn);
    return std::nullopt;
}

// SECTOR_STUFF (8/11/12/13/14): the arrays are 256 x 0x58 at 0x800b60, so the
// slot is index * 0x58 (the handlers compute ``index*11 << 3``).  Evidence:
//   op  8 0x457815  idx<0 || idx>=0x100 -> "bad set_sector_movement sector"
//                   (0x4b8600) and skip; 3x fild dword -> +0x00/+0x08/+0x10;
//                   3x raw 8-byte double -> +0x18/+0x20/+0x28 (0x45786c..ac)
//   op 11 0x457961  +0x40 = value & 0xffff (dword store 0x45799a, upper half
//                   0), +0x44 = value >> 16 arithmetic (0x4579a0, sign
//                   extended); bad index -> silent skip
//   op 12 0x4579ee  call 0x46b740(idx, N, V, D); helper stores N -> +0x48
//                   (sample), V -> +0x4c (volume), D -> +0x30/+0x34; the
//                   signature is LVND, but the handler pushes [8] (N) before
//                   [4] (V) (0x4579fb/0x4579f6, pushes at 0x457a04/0x457a03).
//                   bad index -> 0x4ba864 and no write, then 0x40c030
//                   restarts the audio voices
//   op 13 0x4579ab  +0x38 = E1, +0x3c = E2 (0x4579d6/0x4579e0)
//   op 14 0x457929  +0x50 = value (0x457955, byte-wise 0x800bb0 for slot 0)
// The sector *brightness* and *type* are **not** part of this array: they are
// fields of the world sector record (0x5670d8) and are handled by
// h_world_sector().
std::optional<std::size_t> VmExecutor::h_sector(VmState& state, const VmInsn& insn) {
    const std::vector<SbtValue>& args = insn.args;
    std::size_t wanted = 2; // sector index + at least one operand
    switch (insn.op) {
    case 8: wanted = 7; break;               // LLLLAAA: index + 3L + 3A
    case 11: case 14: wanted = 2; break;     // LL: index + value
    case 12: wanted = 4; break;              // LVND: index + V + N + D
    case 13: wanted = 3; break;              // LEE: index + two events
    // SET_SOURCESECTOR_* (0x1b..0x1f): the sector is the implicit source
    // sector in `edi`, so the operand list starts with the value.
    case 0x1b: wanted = 6; break;            // LLLAAA
    case 0x1d: case 0x1f: wanted = 1; break; // L: value only
    // SET_HANDLESECTOR_* (0x28..0x2d): first operand is the handle.
    case 0x28: wanted = 7; break;            // ELLLAAA
    case 0x2b: case 0x2c: wanted = 2; break; // EL
    case 0x2d: wanted = 4; break;            // ENVD
    default: break;
    }
    if (args.size() < wanted) {
        event(state, insn);
        note(insn.name + " at " + hexs(insn.pc) + ": truncated operands");
        return std::nullopt;
    }

    // Which SECTOR_STUFF slot the command addresses:
    //   SET_SECTOR_*        operand 1 (0x457815 reads it into eax),
    //   SET_SOURCESECTOR_*  edi = the sector the script was triggered from
    //                       (0x457fc6 tests it but never reads an operand),
    //   SET_HANDLESECTOR_*  operand 1 is a handle: the handler scans
    //                       0x5b1df8[0..0x5e3de0) and takes 0x617fb8[i]
    //                       (0x45829e..0x45832d).
    std::size_t first_value = 1;
    int index = -1;
    switch (insn.op) {
    case 0x1b: case 0x1d: case 0x1f:
        index = state.source_sector;
        first_value = 0;
        break;
    case 0x28: case 0x2b: case 0x2c: case 0x2d: {
        const int handle = arg_i32(args[0]);
        // The handle table is walked to the end (0x4582b3..0x45832d has no
        // break): every row with this id is written.
        const std::vector<int> sectors = state.sectors_of_handle(handle);
        if (sectors.empty()) {
            // 0x45832f/0x45899a: the operands are consumed and the command is
            // a no-op for a handle that is not in the table.
            event(state, insn);
            note(insn.name + " at " + hexs(insn.pc) + ": handle " +
                 std::to_string(handle) + " is not registered");
            return std::nullopt;
        }
        for (const int sector : sectors) {
            write_sector_stuff(state, insn, sector, args, first_value);
        }
        event(state, insn);
        return std::nullopt;
    }
    default:
        index = arg_i32(args[0]);
        break;
    }
    if (index < 0 || index >= static_cast<int>(kSbtSectorSlots)) {
        // Original: movement/sound print an error, damage/flags/events consume
        // the operands and continue; none of them write out of bounds.
        event(state, insn);
        note(insn.name + " at " + hexs(insn.pc) + ": sector " +
             std::to_string(index) + " out of range");
        return std::nullopt;
    }
    write_sector_stuff(state, insn, index, args, first_value);

    event(state, insn);
    return std::nullopt;
}

// One SECTOR_STUFF slot write (the shared body of the SET_SECTOR_*,
// SET_SOURCESECTOR_* and SET_HANDLESECTOR_* commands).
void VmExecutor::write_sector_stuff(VmState& state, const VmInsn& insn, int index,
                                    const std::vector<SbtValue>& args,
                                    std::size_t first_value) {
    if (index < 0 || index >= static_cast<int>(kSbtSectorSlots)) {
        // The plain and the source variants check the index (0x457815,
        // 0x457fc6); the handle variants do not (0x4582bc) and would write
        // outside the array in the original.
        note(insn.name + " at " + hexs(insn.pc) + ": sector " +
             std::to_string(index) + " out of range");
        return;
    }
    VmSectorSlot& slot = state.sectors[static_cast<std::size_t>(index)];
    switch (insn.op) {
    case 8:  // SET_SECTOR_MOVEMENT
    case 0x1b: // SET_SOURCESECTOR_MOVEMENT (0x457fc6, same body, edi = sector)
    case 0x28: // SET_HANDLESECTOR_MOVEMENT (0x45829b, same body, handle)
        for (std::size_t k = 0; k < 3; ++k) {
            slot.move[k] = static_cast<double>(arg_i32(args[first_value + k]));
            slot.extra[k] = arg_f64(args[first_value + 3 + k]);
        }
        break;
    case 11:   // SET_SECTOR_DAMAGE: one operand split into +0x40/+0x44
    case 0x1f: // SET_SOURCESECTOR_DAMAGE (0x4580cf)
    case 0x2b: // SET_HANDLESECTOR_DAMAGE (0x458486)
    {
        const std::int32_t value = arg_i32(args[first_value]);
        slot.damage_a = static_cast<std::uint32_t>(value) & 0xffffu;
        slot.damage_b = value >> 16; // arithmetic shift, sign-extended
        break;
    }
    case 12:   // SET_SECTOR_SOUND (LVND): the handler passes (N, V, D)
        slot.sound = arg_i32(args[first_value + 1]);      // N -> +0x48
        slot.sound_count = arg_i32(args[first_value]);    // V -> +0x4c
        slot.sound_param = arg_f64(args[first_value + 2]);
        break;
    case 0x2d: // SET_HANDLESECTOR_SOUND (ENVD, 0x45833a): (N, V, D) too
        slot.sound = arg_i32(args[first_value]);          // N -> +0x48
        slot.sound_count = arg_i32(args[first_value + 1]); // V -> +0x4c
        slot.sound_param = arg_f64(args[first_value + 2]);
        break;
    case 13: // SET_SECTOR_EVENTS
        slot.event_a = arg_i32(args[first_value]);
        slot.event_b = arg_i32(args[first_value + 1]);
        break;
    case 14:   // SET_SECTOR_FLAGS
    case 0x1d: // SET_SOURCESECTOR_FLAGS (0x458111, edi = sector)
    case 0x2c: // SET_HANDLESECTOR_FLAGS (0x4584e7, handle)
        slot.flags = arg_u32(args[first_value]);
        break;
    default:
        break;
    }
}

// The world-sector family: SET_SECTOR_BRIGHTNESS (0x4578d6),
// SET_SECTOR_TYPE (0x457a1c) and their source/handle twins write the runtime
// sector array 0x5670d8 (count 0x5670dc, stride 0x18) — a different array
// than SECTOR_STUFF (0x800b60).
//
//   op  9 0x4578d6  the index is operand 0; the getter 0x42d8a5 returns NULL
//                   outside 0..count-1 -> "Script run error: bad set_sector_%ld."
//                   (0x457909); the light byte (sector+0x16) is the LOW BYTE of
//                   operand 1 (0x4578f8)
//   op 10 0x457a1c  the setter 0x42d9d5 stores (operand 1 * 6) as a u16 at
//                   sector+0x14 and returns -1 outside the table (no message)
//   op 28 0x458063  the source sector (edi) instead of an index; the getter
//                   0x42d8a5 fails -> "bad set_sector_%ld" (0x45808d)
//   op 30 0x4580a9  the source sector; 0x4580a9 rejects everything outside
//                   0..0xff (jump to 0x45c36a) before 0x42d9d5 runs
//   op 41 0x4583a6  the sector handle table (0x5b1df8/0x617fb8) is walked to
//                   the end: EVERY row with the id is written
//   op 42 0x458429  the same, for the type
std::optional<std::size_t> VmExecutor::h_world_sector(VmState& state, const VmInsn& insn) {
    const std::vector<SbtValue>& args = insn.args;
    const bool source = (insn.op == 0x1c || insn.op == 0x1e);
    const bool handle = (insn.op == 0x29 || insn.op == 0x2a);
    const bool brightness = (insn.op == 9 || insn.op == 0x1c || insn.op == 0x29);

    // Operand layout: (index, value) | (value) | (handle, value).
    const std::size_t wanted = source ? 1 : 2;
    if (args.size() < wanted) {
        event(state, insn);
        note(insn.name + " at " + hexs(insn.pc) + ": truncated operands");
        return std::nullopt;
    }
    const int value = arg_i32(args[source ? 0 : 1]);

    std::vector<int> sectors;
    if (handle) {
        sectors = state.sectors_of_handle(arg_i32(args[0]));
    } else if (source) {
        sectors.push_back(state.source_sector);
    } else {
        sectors.push_back(arg_i32(args[0]));
    }

    for (const int sector : sectors) {
        // The source variant of the type rejects everything outside 0..0xff
        // before it even looks at the sector table (0x4580a9).
        if (source && insn.op == 0x1e && (sector < 0 || sector >= 0x100)) {
            continue;
        }
        VmWorldSector* record = state.world_sector(sector);
        if (record == nullptr) {
            if (brightness) {
                // 0x457909 / 0x45808d / 0x4583f6 print
                // "Script run error: bad set_sector_%ld." and write nothing.
                note("Script run error: bad set_sector_" + std::to_string(sector) + ".");
            }
            continue;
        }
        if (brightness) {
            record->light = static_cast<std::uint8_t>(value & 0xff);
        } else {
            // 0x42d9d5: `imul ecx,ecx,6` and a u16 store into sector+0x14.
            // The multiply wraps like the 32-bit `imul` of the original.
            const std::uint32_t scaled = static_cast<std::uint32_t>(value) * 6u;
            record->kind = static_cast<std::uint16_t>(scaled & 0xffffu);
        }
    }

    event(state, insn);
    return std::nullopt;
}

// The world-polygon family: SET_POLY_TEXTURE (0x457a40),
// SET_POLY_BRIGHTNESS (0x457a79), SET_POLY_SCROLL (0x457e7e) and their
// source/handle twins write the 0x80-byte polygon records of the world,
// reached through the getter 0x434463 (sector, object, poly).
//
//   op 15 0x457a40  (sector, object, poly, texture) -> poly+0x00 (0x457a68)
//   op 16 0x457a79  (sector, object, poly, light)   -> poly+0x70 (0x457aa0)
//   op 17 0x457e7e  (sector, object, poly, ...)     -> poly+0x5c/+0x60
//   op 32 0x458178  the source polygon (edi, [ebp+0x14], [ebp+0x18])
//   op 33 0x4581ab  -> poly+0x70
//   op 34 0x458140  -> poly+0x02
//   op 35 0x45821e  -> poly+0x5c/+0x60
//   op 46 0x45853a  the poly handle table (0x5b21f8/0x618b88/0x5e5680/0x6183b8)
//   op 47 0x4586d5  -> poly+0x70
//   op 48 0x4587a1  -> poly+0x5c/+0x60
//   op 49 0x458607  -> poly+0x02
std::optional<std::size_t> VmExecutor::h_world_poly(VmState& state, const VmInsn& insn) {
    const std::vector<SbtValue>& args = insn.args;
    const bool source = (insn.op >= 0x20 && insn.op <= 0x23);
    const bool handle = (insn.op >= 0x2e && insn.op <= 0x31);
    const bool scroll = (insn.op == 0x11 || insn.op == 0x23 || insn.op == 0x30);

    // Operand layout: (sector, object, poly, value[, value]) |
    // (value[, value]) | (handle, value[, value]).
    const std::size_t wanted = source ? (scroll ? 4 : 1) : (handle ? (scroll ? 5 : 2) : (scroll ? 7 : 4));
    if (args.size() < wanted) {
        event(state, insn);
        note(insn.name + " at " + hexs(insn.pc) + ": truncated operands");
        return std::nullopt;
    }

    // Where the value operands start.
    const std::size_t value_at = source ? 0 : (handle ? 1 : 3);

    std::vector<std::array<int, 3>> targets;
    if (handle) {
        for (const VmPolyHandle& row : state.polys_of_handle(arg_i32(args[0]))) {
            targets.push_back({row.sector, row.object, row.poly});
        }
    } else if (source) {
        targets.push_back({state.source_sector, state.source_object, state.source_poly});
    } else {
        targets.push_back({arg_i32(args[0]), arg_i32(args[1]), arg_i32(args[2])});
    }

    for (const auto& target : targets) {
        VmWorldPoly* record = state.world_poly(target[0], target[1], target[2]);
        if (record == nullptr) {
            // 0x457ab1 / 0x4581dd / 0x4585a9 print "Script run error: unable to
            // obtain poly %ld %ld %ld." and write nothing.
            note("Script run error: unable to obtain poly " + std::to_string(target[0]) +
                 " " + std::to_string(target[1]) + " " + std::to_string(target[2]) + ".");
            continue;
        }
        switch (insn.op) {
        case 0x0f: // SET_POLY_TEXTURE
        case 0x20: // SET_SOURCEPOLY_TEXTURE
        case 0x2e: // SET_HANDLEPOLY_TEXTURE
            record->texture = static_cast<std::int16_t>(arg_i32(args[value_at]) & 0xffff);
            break;
        case 0x10: // SET_POLY_BRIGHTNESS
        case 0x21: // SET_SOURCEPOLY_BRIGHTNESS
        case 0x2f: // SET_HANDLEPOLY_BRIGHTNESS
            record->brightness = static_cast<std::uint8_t>(arg_i32(args[value_at]) & 0xff);
            break;
        case 0x22: // SET_SOURCEPOLY_MATERIAL
        case 0x31: // SET_HANDLEPOLY_MATERIAL
            record->material = static_cast<std::uint16_t>(arg_i32(args[value_at]) & 0xffff);
            break;
        case 0x11: // SET_POLY_SCROLL
        case 0x23: // SET_SOURCEPOLY_SCROLL
        case 0x30: // SET_HANDLEPOLY_SCROLL
            record->scroll_u = static_cast<std::int16_t>(arg_i32(args[value_at]) & 0xffff);
            record->scroll_v = static_cast<std::int16_t>(arg_i32(args[value_at + 1]) & 0xffff);
            break;
        default:
            break;
        }
    }

    event(state, insn);
    return std::nullopt;
}

// -- state ----------------------------------------------------------------

std::optional<std::size_t> VmExecutor::h_camera_haze(VmState& state,
                                                    const VmInsn& insn) {
    // 0x45b3bf: arg1 -> f64 0x80f9a8 (no clamp), arg2 > 0 -> 0x5e3dd8 =
    // arg2 * 0.91 (0x45b3fa, constant 0x4a2578), otherwise the rate is 0.
    const auto start = insn.args.size() > 0 ? arg_f64(insn.args[0]) : 0.0;
    const auto rate = insn.args.size() > 1 ? arg_f64(insn.args[1]) : 0.0;
    state.haze = start;
    state.haze_rate = rate > 0.0 ? rate * 0.91 : 0.0;
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_level(VmState& state, const VmInsn& insn) {
    state.level[insn.name] = insn.args;
    if (insn.name == "SET_LEVEL_NAME" && !insn.args.empty()) {
        const SbtValue& value = insn.args.at(0);
        state.level_name = value.kind == SbtValue::Kind::String ? value.s
                                                               : value_text(value);
    }
    if (insn.name == "SET_LEVEL_COUNTDOWN_TIMER" && !insn.args.empty()) {
        state.level["countdown"] = {insn.args.at(0)};
    }
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_music(VmState& state, const VmInsn& insn) {
    const auto bank = [&]() { return arg_i32(insn.args.at(0)); };
    if (insn.name == "LOAD_MUSIC" && insn.args.size() >= 2) {
        const SbtValue& value = insn.args.at(1);
        state.music_banks[bank()] =
            value.kind == SbtValue::Kind::String ? value.s : value_text(value);
    } else if (insn.name == "PLAY_MUSIC" && !insn.args.empty()) {
        state.music_bank = bank();
        state.music_playing = true;
    } else if (insn.name == "STOP_MUSIC") {
        state.music_playing = false;
    } else if (insn.name == "UNLOAD_MUSIC" && !insn.args.empty()) {
        state.music_banks.erase(bank());
        if (state.music_bank == bank()) {
            state.music_playing = false;
        }
    } else if (insn.name == "MEMORIZE_CURRENT_MUSIC_BANK") {
        state.music_memorized = state.music_bank;
    } else if (insn.name == "PLAY_MEMORIZED_MUSIC_BANK" && state.music_memorized) {
        state.music_bank = state.music_memorized;
        state.music_playing = true;
    }
    event(state, insn); // same event the generic action logged before
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_screen_text(VmState& state,
                                                     const VmInsn& insn) {
    int slot = 0;
    std::string text;
    for (const SbtValue& value : insn.args) {
        if (value.kind == SbtValue::Kind::String) {
            text = value.s;
        } else {
            slot = arg_i32(value);
        }
    }
    state.screen_text.emplace_back(slot, text);
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_screen(VmState& state, const VmInsn& insn) {
    int& occ = state.occ[insn.pc];
    state.screens.push_back(VmState::ScreenRequest{
        insn.name, insn.args, static_cast<std::uint32_t>(insn.pc), occ});
    ++occ;
    event(state, insn);
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_add_ammo(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    const int type = arg_i32(insn.args.at(0));
    const int delta = arg_i32(insn.args.at(1));
    int value = player->ammo[type] + delta;
    if (value < 0) {
        value = 0;
    }
    const auto cap = player->ammo_max.find(type);
    if (cap != player->ammo_max.end() && value > cap->second) {
        value = cap->second;
    }
    player->ammo[type] = value;
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_cash(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->cash += arg_i32(insn.args.at(0));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_heal(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    const int amount = insn.args.size() > 1 ? arg_i32(insn.args.at(1))
                                            : arg_i32(insn.args.at(0));
    if (insn.name == "REPAIR_SOURCEPLAYER") {
        player->shield = std::min(player->max_shield, player->shield + amount);
    } else {
        player->health = std::min(player->max_health, player->health + amount);
    }
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_equip(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    const int bit = arg_i32(insn.args.at(0));
    const int on = arg_i32(insn.args.at(1));
    if (bit < 0 || bit > 31) {
        return std::nullopt;
    }
    if (on != 0) {
        player->equipment_active |= 1u << bit;
    } else {
        player->equipment_active &= ~(1u << bit);
    }
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_index(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    if (insn.name == "SOURCEPLAYER_WEAPON_INDEX") {
        player->weapon_index = arg_i32(insn.args.at(0));
    } else {
        player->equipment_index = arg_i32(insn.args.at(0));
    }
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_playertype(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->playertype = arg_i32(insn.args.at(0));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_movement(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->movement_type = static_cast<int>(arg_u32(insn.args.at(0)));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_salvage(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->salvage += arg_i32(insn.args.at(0));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_morale(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->morale += arg_i32(insn.args.at(0));
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_secret(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    ++player->secrets;
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_source_clear(VmState& state, const VmInsn& insn) {
    state.source_entity.reset();
    state.source_player.reset();
    event(state, insn, "source cleared");
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_kill(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    player->health = 0.0;
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_def(VmState& state, const VmInsn& insn) {
    state.defs[insn.name] = insn.args;
    if (insn.name == "DEFINE_SHIELDS" && !insn.args.empty()) {
        const double shield = static_cast<double>(arg_i32(insn.args.at(0)));
        for (auto& [index, player] : state.players) {
            (void)index;
            player.max_shield = shield;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> VmExecutor::h_bits(VmState& state, const VmInsn& insn) {
    VmPlayer* player = state.player();
    if (player == nullptr) {
        event(state, insn, "no source player");
        return std::nullopt;
    }
    const std::uint32_t operand = arg_u32(insn.args.at(0));
    const bool is_and = insn.name.rfind("AND", 0) == 0;
    auto apply = [&](std::uint32_t& field) {
        field = is_and ? (field & operand) : (field | operand);
    };
    if (insn.name.find("KEY_BITS") != std::string::npos) {
        apply(player->key_bits);
    } else if (insn.name.find("WEAPON_BITS") != std::string::npos) {
        apply(player->weapon_bits);
    } else if (insn.name.find("EQUIPMENT_BITS") != std::string::npos) {
        apply(player->equipment_bits);
    } else if (insn.name.find("BEHAVIOUR_BITS") != std::string::npos) {
        apply(player->behaviour);
    } else {
        apply(player->extra_bits);
    }
    return std::nullopt;
}

} // namespace gm
