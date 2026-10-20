#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "res/sbt.h"
#include "res/sbtvm.h"

namespace gm {

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

/// Why an explored path stopped (``gm/sbtvm_exec.py``'s ``PathResult.reason``).
enum class VmReason {
    End,                // 0xff at call depth 0
    Return,             // EXIT_EVENT / 0xff inside a sub-script
    UnknownOpcode,
    UnsupportedCommand,
    UnknownCondition,
    StepLimit,
    PathLimit,
    Deadline,
    BadOperand,
    BadPC,
    StackOverflow,
    Recursion,
    ProgramExit,
    LevelTransition,
};

/// ``end``, ``return``, ``unknown-opcode``, ... (the Python string).
std::string_view vm_reason_name(VmReason reason);

/// ``end``/``return``/``program-exit``/``level-transition``.
bool vm_reason_ok(VmReason reason);

// ---------------------------------------------------------------------------
// Instruction decoding
// ---------------------------------------------------------------------------

enum class VmInsnKind {
    End,        // 0xff
    If,         // 199: condition + sizing dword + then body + optional ELSE
    Else,       // 200: relative forward jump
    Eval,       // 0xc9: bare condition statement
    Cmd,        // dispatch-table command
    Table,      // DEFINE_WEAPONS / AMMO / PLAYER_TYPES / EQUIPMENT
    Objectives, // DEFINE_LEVEL_OBJECTIVES
    Path,       // SOURCEPLAYER_PATH
    Lift,       // ADD_LIFT
    Unknown,    // no command table entry
};

/// One instruction decoded on demand at a byte PC.  Mirrors the dataclass in
/// ``gm/sbtvm_exec.py``; the bodies of IF are *not* decoded recursively here
/// (the executor walks raw byte PCs, exactly like the engine).
struct VmInsn {
    std::uint32_t pc = 0;
    std::uint8_t op = 0;
    std::string name;
    std::vector<SbtValue> args;
    std::size_t next = 0;
    VmInsnKind kind = VmInsnKind::Cmd;

    std::string condition;
    std::vector<SbtValue> cond_args;
    std::size_t body_start = 0;
    std::size_t body_end = 0;
    std::size_t target = 0;

    std::vector<SbtTableRecord> table;
    std::vector<std::uint32_t> ints;                  // SOURCEPLAYER_PATH points
    std::vector<std::uint8_t> lift_head;              // ADD_LIFT 0x94-byte head
    std::vector<std::array<std::uint32_t, 3>> stops;  // ADD_LIFT stops
    std::string error;
};

/// Decode the instruction at *pc*.  Returns nullopt with *error* set when the
/// PC is outside the block or an operand is truncated/unterminated.
std::optional<VmInsn> vm_decode_at(std::span<const std::uint8_t> code, std::size_t pc,
                                   std::string& error);

enum class VmCommandStatus {
    Structural,
    Flow,
    Scene,
    Attached,
    State,
    Event,
    Unsupported,
    Unknown,
};

/// Classification of command *op* (same rules as ``command_status``).
VmCommandStatus vm_command_status(std::uint8_t op);
std::string_view vm_command_status_name(VmCommandStatus status);

struct VmCommandInfo {
    std::uint8_t op = 0;
    std::string_view name;
    std::string_view sig; // empty: no operands
};

/// Look up a command / condition by name (used by tests to encode byte code).
std::optional<VmCommandInfo> vm_command_info(std::string_view name);
std::optional<VmCommandInfo> vm_condition_info(std::string_view name);

// ---------------------------------------------------------------------------
// State model
// ---------------------------------------------------------------------------

struct VmEntity {
    int id = 0;
    std::uint32_t flags = 0x3FF;
    std::uint32_t offset = 0; // sub-script byte offset

    bool enabled() const { return (flags & 0x1FFu) != 0; }
};

struct VmPlayer {
    int index = 0;
    int playertype = 0;
    std::uint32_t behaviour = 0; // +0x410
    std::uint32_t key_bits = 0;
    std::uint32_t weapon_bits = 0; // +0x2d8
    std::uint32_t equipment_bits = 0;
    std::uint32_t extra_bits = 0;
    int cash = 0; // +0x36c
    double health = 100.0;
    double shield = 0.0;
    double max_health = 100.0;
    double max_shield = 100.0;
    std::map<int, int> ammo;     // +0x2dc + 4*type
    std::map<int, int> ammo_max;
    std::uint32_t equipment_active = 0; // +0x368
    int weapon_index = 0;               // +0x2d4
    int equipment_index = 0;
    int movement_type = 0;
    int salvage = 0;
    int morale = 0;
    int secrets = 0;
};

struct VmLight {
    std::string kind; // "sky" | "local"
    std::array<int, 3> pos{};
    std::array<int, 3> rgb{255, 255, 255};
    int param = 0;
    std::vector<int> extra;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmLocation {
    int thing_id = 0;
    std::array<int, 3> pos{};
    std::array<double, 3> orientation{};
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmItem {
    int id = 0;
    std::array<int, 3> pos{};
    bool fixed = false;
    bool named = false;
    std::vector<SbtValue> extra; // raw operands, like the Python tuple
    std::uint32_t site = 0;
    int occ = 0;
};

/// A sprite/item positioned by a runtime source (no world position yet).
struct VmAttached {
    std::string kind;
    std::vector<SbtValue> args;
    std::optional<int> source;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmSound {
    std::array<int, 3> pos{};
    int sample = 0;
    int volume = 0;
    double pitch = 0.0;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmSprite {
    std::array<double, 3> pos{};
    int jitter = 0;
    int id = 0;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmThing {
    std::optional<int> entity;
    std::string kind;
    int type_index = 0;
    std::optional<std::array<int, 3>> pos; // only ADD_THING / ADD_THING_EX
    std::vector<double> angles;
    int owner = 0;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmDoor {
    int flags = 0;
    std::array<int, 5> ids{};
    std::array<double, 3> angles{};
    std::array<int, 7> params{};
    std::array<double, 9> values{};
    std::array<int, 6> tail{};
    int toggles = 0; // GENERATE on this door id
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmSwitch {
    int kind = 0;
    int id = 0;
    int radius = 0;
    std::array<int, 3> pos{};
    std::array<int, 2> extra{};
    int triggers = 0;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmLift {
    std::vector<std::uint8_t> head;
    std::vector<std::array<std::uint32_t, 3>> stops;
    std::uint32_t site = 0;
    int occ = 0;
};

struct VmSpawn {
    int index = 0;
    std::array<int, 3> pos{};
    std::array<double, 3> angles{};
    std::string kind = "campaign"; // "campaign" | "deathmatch"
    std::uint32_t site = 0;
    int occ = 0;
};

/// Everything the executed script builds.  Category names and order match
/// ``gm.sbtvm_exec.Scene.counts()``.
struct VmScene {
    std::vector<VmLight> lights;
    std::vector<VmLocation> locations;
    std::vector<VmItem> items;
    std::vector<VmItem> named_items;
    std::vector<VmAttached> attached;
    std::vector<VmSound> sounds;
    std::vector<VmSprite> sprites;
    std::vector<VmThing> things;
    std::vector<VmDoor> doors;
    std::vector<VmSwitch> switches;
    std::vector<VmLift> lifts;
    std::vector<VmSpawn> spawns;

    std::map<std::string, std::size_t> counts() const;
    void extend(const VmScene& other);
};

struct VmEvent {
    std::uint32_t pc = 0;
    std::string name;
    std::vector<SbtValue> args;
    std::string note;
};

struct VmTraceEvent {
    std::uint32_t pc = 0;
    int depth = 0;
    std::string name;
    std::vector<SbtValue> args;
    std::string note;
};

struct VmStackFrame {
    std::size_t return_pc = 0;
    std::optional<int> source;
};

struct VmState {
    std::span<const std::uint8_t> code;

    std::map<int, VmEntity> entities; // id -> entity
    std::vector<int> order;           // ids in table order
    std::map<int, VmPlayer> players;  // index -> player

    std::optional<int> source_player;
    std::optional<int> source_entity;
    std::optional<int> source_alt;

    int game_mode = 0; // 0 campaign, 1 dm-normal, 2 dm-classic, 5 scavenger
    int mp_flags = 0;  // bit0 team, bit1 CTF
    int player_count = 1;
    int difficulty = 1;
    int level_number = 0;
    bool god_mode = false;

    std::string level_name;
    std::map<std::string, std::vector<SbtValue>> level;
    std::vector<std::string> objectives;
    std::uint32_t objectives_bits = 0;
    std::map<std::string, std::vector<SbtValue>> defs;             // DEFINE_* args
    std::map<std::string, std::vector<SbtTableRecord>> table_defs; // DEFINE_* tables
    std::vector<std::uint32_t> path;

    VmScene scene;
    std::vector<VmEvent> events;

    std::vector<VmStackFrame> stack;
    std::vector<std::size_t> active;

    std::size_t pc = 0;
    int clock = 0;
    std::uint32_t rng = 0x12345678;
    std::map<std::size_t, int> occ;
    int steps = 0;
    std::vector<VmTraceEvent> trace;

    /// The source player (or *index*), created on demand like ``State.player``.
    VmPlayer* player();
    VmPlayer* player(int index);

    /// ``rand() & 0xfff`` scaled by 1/4095 (constant 0x4a2580).
    double rnd();
};

/// Context used to build a state from a parsed level (``state_from_script``).
struct VmContext {
    int game_mode = 0;
    int mp_flags = 0;
    int player_count = 1;
    int difficulty = 1;
    int level_number = 0;
    bool god_mode = false;
    int players = 1;
    std::uint32_t seed = 0x12345678;
    std::optional<int> playertype;
    std::optional<int> source_player;
};

/// State for a ``.sbt`` level: players plus the entity table
/// (``n`` rows of flags/id/offset, ``gm.scene.entities``).
VmState vm_state_from_level(const SbtLevel& level, const VmContext& context = {});

struct VmPathResult {
    VmReason reason = VmReason::End;
    std::size_t pc = 0;
    std::string detail;
    int steps = 0;
    VmState state;

    bool ok() const { return vm_reason_ok(reason); }
};

/// Union of every path's scene, deduplicated by ``(category, site, occ)``.
VmScene vm_merge_scenes(const std::vector<VmPathResult>& results);

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

enum class VmUnknownPolicy { Stop, False, True, Both };

struct VmOptions {
    int max_steps = 200000;
    double max_time = 5.0; // seconds; 0 disables the wall-clock budget
    int max_paths = 64;
    VmUnknownPolicy on_unknown = VmUnknownPolicy::Both;
    bool follow_generate = true;
    bool random_unknown = false;
    bool trace = false;
    std::size_t trace_limit = 5000;
};

/// Run SCRIPT byte code over a :class:`VmState`.
///
/// The loop and the uncertainty policy are ported from
/// ``gm/sbtvm_exec.Executor``: no operand stack, conditions are handlers that
/// read inline operands, ``GENERATE`` calls a sub-script, ``IF`` false skips
/// the sizing dword (landing on the else code), and an unknown opcode/command
/// stops the path with a reason instead of being skipped.
class VmExecutor {
public:
    explicit VmExecutor(VmState state, VmOptions options = {});

    /// Execute from *start*, one result per explored path.  *source_entity*
    /// overrides the state's source when present (the Python ``run`` argument).
    std::vector<VmPathResult> run(std::size_t start = 0,
                                  std::optional<int> source_entity = std::nullopt);

    const std::vector<std::string>& notes() const { return notes_; }
    const std::map<std::uint8_t, int>& command_counts() const { return command_counts_; }
    const std::map<std::string, int>& condition_counts() const { return condition_counts_; }

private:
    using Frontier = std::deque<std::pair<VmState, std::size_t>>;

    struct Stop {
        VmReason reason = VmReason::End;
        std::size_t pc = 0;
        std::string detail;
    };

    Stop run_path(VmState& state, std::size_t pc, Frontier& frontier);
    std::size_t do_if(VmState& state, const VmInsn& insn, Frontier& frontier);
    std::optional<bool> eval(VmState& state, const VmInsn& insn, bool as_if);
    std::optional<bool> check(VmState& state, int cid, const std::vector<SbtValue>& args);
    void action(VmState& state, int cid, const std::string& name,
                const std::vector<SbtValue>& args, const VmInsn& insn);
    void structural(VmState& state, const VmInsn& insn);
    void thing(VmState& state, const std::string& name,
               const std::vector<SbtValue>& args, const VmInsn& insn);

    template <typename T>
    void add(VmState& state, const VmInsn& insn, std::vector<T>& out, T object) {
        const auto site = static_cast<std::uint32_t>(insn.pc);
        int& occ = state.occ[insn.pc];
        object.site = site;
        object.occ = occ;
        ++occ;
        out.push_back(std::move(object));
    }

    void event(VmState& state, const VmInsn& insn, const std::string& note = "");
    void trace(VmState& state, const VmInsn& insn, const std::string& note = "");
    void note(std::string message);
    bool expired() const;

    void install_handlers();

    // flow
    std::optional<std::size_t> h_exit_event(VmState&, const VmInsn&);
    std::optional<std::size_t> h_exit_program(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sleep(VmState&, const VmInsn&);
    std::optional<std::size_t> h_generate(VmState&, const VmInsn&);
    std::optional<std::size_t> h_transition(VmState&, const VmInsn&);
    // entity flags
    std::optional<std::size_t> h_enable(VmState&, const VmInsn&);
    std::optional<std::size_t> h_disable(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sp_enable(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sp_disable(VmState&, const VmInsn&);
    std::optional<std::size_t> h_or_objectives(VmState&, const VmInsn&);
    // scene
    std::optional<std::size_t> h_sky(VmState&, const VmInsn&);
    std::optional<std::size_t> h_light(VmState&, const VmInsn&);
    std::optional<std::size_t> h_location(VmState&, const VmInsn&);
    std::optional<std::size_t> h_delete_location(VmState&, const VmInsn&);
    std::optional<std::size_t> h_item(VmState&, const VmInsn&);
    std::optional<std::size_t> h_named_item(VmState&, const VmInsn&);
    std::optional<std::size_t> h_named_item_ex(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sound(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sprite(VmState&, const VmInsn&);
    std::optional<std::size_t> h_door(VmState&, const VmInsn&);
    std::optional<std::size_t> h_switch(VmState&, const VmInsn&);
    std::optional<std::size_t> h_spawn(VmState&, const VmInsn&);
    std::optional<std::size_t> h_attached(VmState&, const VmInsn&);
    std::optional<std::size_t> h_event(VmState&, const VmInsn&);
    // state
    std::optional<std::size_t> h_level(VmState&, const VmInsn&);
    std::optional<std::size_t> h_add_ammo(VmState&, const VmInsn&);
    std::optional<std::size_t> h_cash(VmState&, const VmInsn&);
    std::optional<std::size_t> h_heal(VmState&, const VmInsn&);
    std::optional<std::size_t> h_equip(VmState&, const VmInsn&);
    std::optional<std::size_t> h_index(VmState&, const VmInsn&);
    std::optional<std::size_t> h_playertype(VmState&, const VmInsn&);
    std::optional<std::size_t> h_movement(VmState&, const VmInsn&);
    std::optional<std::size_t> h_salvage(VmState&, const VmInsn&);
    std::optional<std::size_t> h_morale(VmState&, const VmInsn&);
    std::optional<std::size_t> h_secret(VmState&, const VmInsn&);
    std::optional<std::size_t> h_source_clear(VmState&, const VmInsn&);
    std::optional<std::size_t> h_kill(VmState&, const VmInsn&);
    std::optional<std::size_t> h_def(VmState&, const VmInsn&);
    std::optional<std::size_t> h_bits(VmState&, const VmInsn&);

    using Handler = std::optional<std::size_t> (VmExecutor::*)(VmState&, const VmInsn&);

    VmState initial_;
    VmOptions options_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::vector<std::string> notes_;
    std::map<std::uint8_t, int> command_counts_;
    std::map<std::string, int> condition_counts_;
    std::map<std::uint8_t, Handler> handlers_;
};

} // namespace gm
