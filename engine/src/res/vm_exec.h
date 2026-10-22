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

#include "res/sbw.h"
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

/// One row of the entity table (docs/ENTITY_FIELDS.md §3): ``flags``, ``id``
/// and the sub-script ``offset``.  There is nothing else in a row.
struct VmEntity {
    int id = 0;
    std::uint32_t flags = 0x3FF;
    std::uint32_t offset = 0; // sub-script byte offset

    /// Any source may run this row (``flags & 0x1ff``: a player bit, 0x100 or
    /// both).  A fired one-shot (0x200 only) and a ``DISABLED`` row are not.
    bool enabled() const { return (flags & 0x1FFu) != 0; }
    /// Bit 9: 0x45fa95 `TEST AH,0x2`; cleared against 0x1ff at 0x45fa9e.
    bool one_shot() const { return (flags & 0x200u) != 0; }
};

/// One slot of the sleeping-script table (docs/ENTITY_FIELDS.md §8).  The
/// engine keeps nine parallel 256-entry arrays; a slot is free when the
/// *offset* array holds a negative value (`0x5e3df0`, checked at 0x45c442
/// and freed by writing -1 at 0x45b962 / 0x45731d).
struct VmSleepSlot {
    std::int32_t offset = -1; ///< 0x5e3df0 continuation PC (-1: free)
    std::int32_t ticks = 0;   ///< 0x5e4278 remaining ticks
    /// 0x616258, 0x5e5e58, 0x5e2dd8, 0x5e39d8, 0x5e35d8, 0x5e31d8: the six
    /// opaque ``0x457390`` arguments (param 3..8) saved by SLEEP and handed
    /// back by the ticker 0x457230.
    std::array<std::uint32_t, 6> params{};
    std::uint32_t source = 0xFFFFFFFFu; ///< 0x5b29c8 source handle (param 9)

    bool active() const { return offset >= 0; }
};

/// ``0x400``: the loader cap of the sleeping arrays (0x462cxx reads nine
/// blocks of 0x400 bytes; the SLEEP scan stops at 0x5e41f0 = 0x5e3df0+0x400).
constexpr std::size_t kVmSleepSlots = 256;

/// A sleeping script the ticker woke: continue *offset* with *carry* ticks
/// already elapsed (0x457230 calls 0x457390(offset, -remaining, ...)).
struct VmWake {
    std::uint32_t offset = 0;
    int carry = 0;
    std::uint32_t source = 0xFFFFFFFFu;
    std::array<std::uint32_t, 6> params{};
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
    // -1 = no equipment selected: the original initialises +0x3320 to -1
    // (docs/EQUIPMENT.md section 5); 0 would select the first record at load.
    int equipment_index = -1;
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

/// One SECTOR_STUFF slot as the VM sees it: the runtime array 0x800b60,
/// 256 x 0x58 (docs/RECORDS.md §2).  Only the fields the SET_SECTOR_* handlers
/// write are modelled; ``vm_load_sector_stuff`` seeds the whole view from a
/// parsed block 8 / record 8, whose ``res::SbtSectorSlot`` types every byte.
struct VmSectorSlot {
    /// +0x00..+0x10: three *integer* operands of SET_SECTOR_MOVEMENT (the
    /// handler sign-extends them with ``fild`` at 0x457834/0x45784c/0x45785c).
    std::array<double, 3> move{};
    /// +0x18..+0x2c: the three ``A`` operands, copied as raw 8-byte doubles
    /// (0x45786c..0x4578ac); no reader found in the build.
    std::array<double, 3> extra{};
    /// +0x30: SET_SECTOR_SOUND ``D`` operand (0x46b774/0x46b77a).
    double sound_param = 0.0;
    /// +0x38/+0x3c: SET_SECTOR_EVENTS ``E`` operands (0x4579d6/0x4579e0).
    int event_a = 0;
    int event_b = 0;
    /// +0x40/+0x44: SET_SECTOR_DAMAGE.  The handler splits one 32-bit operand
    /// (0x457961): ``+0x40 = value & 0xffff`` (dword store at 0x45799a) and
    /// ``+0x44 = value >> 16`` arithmetic (0x4579a0).  Both are read back as
    /// dwords and multiplied by dt at 0x4450f6..0x445105, then handed to the
    /// damage function 0x453b50 as two separate amounts.
    std::uint32_t damage_a = 0;
    std::int32_t damage_b = 0;
    /// +0x48: SET_SECTOR_SOUND ``N``, a SOUND_SAMPLE_TABLE index (0x46b76a).
    int sound = 0;
    /// +0x4c: SET_SECTOR_SOUND ``V`` (volume); ``> 0`` means the loop is
    /// playing (0x46b760 stores it, 0x46c5f3 tests it).
    int sound_count = 0;
    /// +0x50: SET_SECTOR_FLAGS (0x457955).
    std::uint32_t flags = 0;
};

struct VmSpawn {
    int index = 0;
    std::array<int, 3> pos{};
    std::array<double, 3> angles{};
    std::string kind = "campaign"; // "campaign" | "deathmatch"
    std::uint32_t site = 0;
    int occ = 0;
};

/// One **world sector** record: the runtime array ``0x5670d8`` (count
/// ``0x5670dc``, stride ``0x18``), docs/WORLD_FIELDS.md §2.1.  It is a
/// different array than ``SECTOR_STUFF`` (``0x800b60``): this one is the
/// geometry the renderer walks, and it is what ``SET_SECTOR_BRIGHTNESS`` /
/// ``SET_SECTOR_TYPE`` and their source/handle twins write.
///
/// Only the two fields the SCRIPT-VM can reach are modelled:
///  * ``kind``  — ``+0x14``, the sector type multiplied by 6 (setter
///    ``0x42d9d5``, getter ``0x42d99f`` divides it back by 6);
///  * ``light`` — ``+0x16``, the sector light byte (writer ``0x4578f8``).
struct VmWorldSector {
    std::uint16_t kind = 0; ///< +0x14: ``type * 6``
    std::uint8_t light = 0;  ///< +0x16: sector light
};

/// One **world polygon** record: the runtime record of ``0x80`` bytes that
/// lives inside an object (``object+0x6c[i]``), addressed by the
/// ``(sector, object, poly)`` triple of the getter ``0x434463``
/// (docs/WORLD_FIELDS.md §2.4).  Only the fields the SCRIPT-VM can write are
/// modelled:
///
///  * ``texture``   — ``+0x00`` (``0x457a68``, ``0x45819a``, ``0x4585a4``);
///  * ``material``  — ``+0x02`` (``0x458166``, ``0x458671``);
///  * ``scroll_u``  — ``+0x5c`` (``0x457ebd``, ``0x458256``, ``0x458825``);
///  * ``scroll_v``  — ``+0x60`` (``0x457ecf``, ``0x458268``, ``0x45883a``);
///  * ``brightness``— ``+0x70`` (``0x457aa0``, ``0x4581cc``, ``0x45873e``).
struct VmWorldPoly {
    int sector = 0; ///< address: sector index
    int object = 0; ///< address: object index inside the sector
    int poly = 0;   ///< address: polygon index inside the object
    std::int16_t texture = 0;    ///< +0x00
    std::uint16_t material = 0;  ///< +0x02
    std::int16_t scroll_u = 0;   ///< +0x5c
    std::int16_t scroll_v = 0;   ///< +0x60
    std::uint8_t brightness = 0; ///< +0x70
};

/// One ``POLY_HANDLE`` row: the four parallel runtime tables ``0x5b21f8``
/// (handle id), ``0x618b88`` (sector), ``0x5e5680`` (object) and ``0x6183b8``
/// (polygon), count ``0x5e4678``, at most ``0x1f4`` rows.  The loader is
/// ``0x463290`` (format string ``0x4b9640``:
/// ``"POLY_HANDLE\n%ld %ld %ld %ld"``).
struct VmPolyHandle {
    int handle = 0;
    int sector = 0;
    int object = 0;
    int poly = 0;
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
    /// Multi-row ``GENERATE``: 0x45fa58 walks the whole id column and runs
    /// *every* matching row, one after another, so the rows after the first
    /// are launched when the previous sub-script returns.  The scan resumes at
    /// ``row_cursor``; ``site`` is the GENERATE pc (0x45fa00's caller) for
    /// diagnostics and ``carry`` restores the SLEEP carry of the caller.
    std::optional<int> generate_id;
    std::size_t row_cursor = 0;
    std::uint32_t site = 0;
    int carry = 0;
};

struct VmState {
    std::span<const std::uint8_t> code;

    /// **Every** row of the table in table order (``0x5e4680``): duplicate ids
    /// are legal and all of their rows run (249 duplicate rows in the shipped
    /// 36 levels).  ``entity(id)`` returns the first row, which is what
    /// single-row lookups (ENABLE/... and the old map API) see.
    std::vector<VmEntity> entities;
    std::vector<int> order;           // ids in table order (one per row)
    std::map<int, VmPlayer> players;  // index -> player

    /// Sleeping-script table (0x5e3df0/0x5e4278/..., docs/ENTITY_FIELDS.md §8).
    /// Read from the ``.sbt`` SCRIPT tail when its ``flag`` byte is set
    /// (``vm_load_sleeping_scripts``); filled by SLEEP when
    /// ``VmOptions::sleep_queue`` is on and drained by ``vm_tick_sleepers``.
    std::array<VmSleepSlot, kVmSleepSlots> sleeping{};
    /// Ticks already elapsed for the current invocation (the engine's second
    /// ``0x457390`` argument, 0x457390/0x45c43b: ``timer = operand - carry``).
    /// Only a woken script starts with a non-zero carry; GENERATE passes 0.
    int sleep_carry = 0;

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

    /// SECTOR_STUFF runtime array 0x800b60 (256 x 0x58): the state the
    /// SET_SECTOR_* commands write and the sector update (0x470c70 movement,
    /// 0x46c5c0 ambient sound, 0x444166/0x444767 flags) reads.  Seeded from
    /// block 8 by ``vm_load_sector_stuff``; the shipped level files carry no
    /// SECTOR_STUFF record, so a level starts with 256 cleared slots, exactly
    /// like loader 0x46b1b0 followed by a no-op 0x46b7b0.
    std::array<VmSectorSlot, kSbtSectorSlots> sectors{};

    /// Sector the running script was triggered from: the third argument of
    /// ``0x457390`` (``[ebp+0x10]`` at ``0x4573ab``), which the dispatcher
    /// keeps in ``edi`` for the whole call.  ``SET_SOURCESECTOR_*``
    /// (cids 27..31 / ``0x1b..0x1f``) use it as the SECTOR_STUFF index instead
    /// of a sector operand (``0x457fef`` writes ``0x800b60 + edi*0x58``).  In
    /// the engine the value comes from the event that generated the script
    /// (the ``param`` field of the record, which for doors is the sector);
    /// ``-1`` is the original's ``edi < 0`` error path.
    int source_sector = -1;

    /// SECTOR_STUFF "sector handles": the runtime table ``0x5b1df8`` (handle
    /// id) / ``0x617fb8`` (sector), count ``0x5e3de0``, at most ``0x100``
    /// entries.  It is **not** built from a level: ``0x456f50`` only clears the
    /// count and ``0x463820`` appends one pair per ``SECTOR_HANDLE: <h> <s>``
    /// line loaded from a text source, so the shipped levels start empty and
    /// ``SET_HANDLESECTOR_*`` (cids 40..45) find nothing until this is filled.
    std::vector<std::pair<int, int>> sector_handles;

    /// Sector for *handle* (first match, as ``0x4582b3`` scans the arrays in
    /// order), or ``nullopt`` when the handle is not registered.
    std::optional<int> sector_of_handle(int handle) const;

    /// World-sector table (``0x5670d8``): the array the sector brightness /
    /// type family writes.  Its size is the sector count ``0x5670dc``; an
    /// empty table means "no world loaded", and then every write is rejected
    /// exactly like the engine's getter ``0x42d8a5`` does.
    std::vector<VmWorldSector> world_sectors;

    /// World-polygon table (the ``0x80``-byte records): addressed by the
    /// ``(sector, object, poly)`` triple of ``0x434463``.  A polygon that is
    /// not in the table does not exist, and the engine's getter returns NULL
    /// for it ("Script run error: unable to obtain poly %ld %ld %ld").
    std::vector<VmWorldPoly> world_polys;

    /// ``POLY_HANDLE`` rows (``0x5b21f8``/``0x618b88``/``0x5e5680``/
    /// ``0x6183b8``, count ``0x5e4678``).  Like the sector handle table this
    /// one is **not** built from a level: ``0x463290`` appends one row per
    /// ``POLY_HANDLE: <h> <sector> <object> <poly>`` line of a text source.
    std::vector<VmPolyHandle> poly_handles;

    /// The world polygon the running script was triggered from: the fourth and
    /// fifth arguments of ``0x457390`` (``[ebp+0x14]`` / ``[ebp+0x18]``).
    /// ``SET_SOURCEPOLY_*`` (cids 32..35) address the polygon through them
    /// together with ``source_sector``; ``-1`` is the original's "no source
    /// polygon" case (the getter then fails).
    int source_object = -1;
    int source_poly = -1;

    /// The world-sector record *index*, or nullptr when the index is outside
    /// the table — the engine's getter ``0x42d8a5`` returns NULL there.
    VmWorldSector* world_sector(int index);
    const VmWorldSector* world_sector(int index) const;

    /// The world polygon *(sector, object, poly)*, or nullptr (the engine's
    /// getter ``0x434463``).
    VmWorldPoly* world_poly(int sector, int object, int poly);
    const VmWorldPoly* world_poly(int sector, int object, int poly) const;

    /// **Every** sector registered under *handle*.  The original does not stop
    /// at the first match: ``0x4583c2``..``0x45841c`` walks the whole handle
    /// table and writes every row with the requested id.
    std::vector<int> sectors_of_handle(int handle) const;

    /// **Every** polygon registered under *handle* (``0x458701``..``0x458790``
    /// walks the whole poly handle table).
    std::vector<VmPolyHandle> polys_of_handle(int handle) const;

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

    /// The first row with *id* (table order), or nullptr.  The engine has no
    /// single-row lookup: ENABLE/DISABLE and GENERATE walk the whole column,
    /// which is what ``entities`` + ``entity()`` allow here.
    VmEntity* entity(int id);
    const VmEntity* entity(int id) const;
    std::size_t entity_count(int id) const;

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

/// Parse ``SECTOR_HANDLE: <handle> <sector>`` lines into
/// ``state.sector_handles``.  The format string is
/// ``s_SECTOR_HANDLE__ld__ld`` (``0x4b9888``, used by ``0x4638a0``); the reader
/// ``0x463820`` appends one pair per line and enforces the 0x100 limit.  A line
/// with a different prefix, a short line or a non-numeric field is skipped (the
/// original prints an error per bad line and keeps the previous pairs).
std::size_t vm_load_sector_handles(VmState& state, std::string_view text);

/// Parse ``POLY_HANDLE: <handle> <sector> <object> <poly>`` lines into
/// ``state.poly_handles``.  The format string is ``0x4b9640``
/// (``"POLY_HANDLE\n%ld %ld %ld %ld"``), used by ``0x463290``; the reader
/// appends one row per line and enforces the ``0x1f4`` (500) limit
/// (``0x463295``: "Maximum number of poly handles").  A line with a different
/// prefix, a short line or a non-numeric field is skipped.
std::size_t vm_load_poly_handles(VmState& state, std::string_view text);

/// Seed the world tables of *state* from a parsed ``.sbw``.
///
/// The runtime world is the array of sectors ``0x5670d8`` (count
/// ``0x5670dc``) with the polygons inside them; this fills the two views the
/// SCRIPT-VM writes: ``world_sectors`` (the sector light ``+0x16`` is zero at
/// load, ``0x42ae05``; the type ``+0x14`` comes from the file) and
/// ``world_polys`` (the polygon records, addressed by ``(sector, object,
/// poly)``).  An empty level clears both tables.
void vm_load_world(VmState& state, const SbwLevel& level);

/// Seed the SECTOR_STUFF view of *state* from a parsed block 8 / record 8.
///
/// Every byte of a ``res::SbtSectorSlot`` is typed (the dead +0x54 word
/// included), so this is lossless; only the fields the VM can address are kept
/// in ``VmSectorSlot``.  Fewer than 256 entries leave the
/// remaining slots cleared, an empty vector clears all of them — the state
/// loader 0x46b1b0 produces before 0x46b7b0 reads the block.
void vm_load_sector_stuff(VmState& state, std::span<const SbtSectorSlot> slots);

/// Seed the sleeping-script table from the SCRIPT tail (the nine 0x400-byte
/// arrays that follow ``flag != 0``, loader 0x462a80).  File order, each
/// 256 x u32: remaining ticks (0x5e4278), sub-script offset (0x5e3df0), then
/// the six parameters 0x616258/0x5e5e58/0x5e2dd8/0x5e39d8/0x5e35d8/0x5e31d8
/// and the source handle 0x5b29c8.  Anything but 9*0x400 bytes clears the
/// table (all 36 shipped levels carry ``flag = 0``).
void vm_load_sleeping_scripts(VmState& state, std::span<const std::uint8_t> tables);

/// Ticker 0x457230: subtract *ticks* from every occupied slot and return the
/// scripts whose timer ran out, in slot order (the engine resumes them inside
/// the same loop).  A woken slot is freed (offset = -1); the returned carry is
/// the overshoot (ticks elapsed past the deadline), which is what 0x457356
/// passes as ``0x457390``'s second argument.
std::vector<VmWake> vm_tick_sleepers(VmState& state, int ticks);

/// ``STOP_SOURCEPLAYER_SLEEPING_SCRIPTS`` (cid 198, 0x45b93e): free every slot
/// whose saved source handle (low 16 bits) equals *handle*; returns the count.
std::size_t vm_stop_sleeping(VmState& state, std::uint32_t handle);

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
    /// Model SLEEP the way 0x457390 case 7 does (0x45c43b): the rest of the
    /// (sub-)script goes into the 256-slot table and the caller continues,
    /// instead of the exploration shortcut ``clock += n`` that keeps the
    /// remainder inline (default: the shortcut, which is what the Python
    /// reference and the level goldens assume).
    bool sleep_queue = false;
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

    // Row selection of 0x45fa00 (0x45fa58..0x45fb1c) and the frame return
    // path shared by END (0xff) and EXIT_EVENT.
    std::optional<std::uint32_t> next_generate_row(VmState& state, int gid,
                                                   std::size_t& cursor);
    std::size_t enter_generate(VmState& state, int gid, std::uint32_t offset,
                               std::size_t cursor, const VmInsn& insn);
    std::optional<std::size_t> leave_frame(VmState& state);

    // flow
    std::optional<std::size_t> h_exit_event(VmState&, const VmInsn&);
    std::optional<std::size_t> h_exit_program(VmState&, const VmInsn&);
    std::optional<std::size_t> h_sleep(VmState&, const VmInsn&);
    std::optional<std::size_t> h_stop_sleeping(VmState&, const VmInsn&);
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
    // SECTOR_STUFF writes (SET_SECTOR_MOVEMENT/DAMAGE/SOUND/EVENTS/FLAGS)
    std::optional<std::size_t> h_sector(VmState&, const VmInsn&);
    /// One SECTOR_STUFF slot write, shared by the plain/source/handle variants.
    void write_sector_stuff(VmState&, const VmInsn&, int index,
                            const std::vector<SbtValue>& args, std::size_t first_value);
    // World-sector writes (SET_SECTOR_BRIGHTNESS/TYPE and their source/handle
    // twins: the runtime array 0x5670d8, not SECTOR_STUFF).
    std::optional<std::size_t> h_world_sector(VmState&, const VmInsn&);
    // World-polygon writes (SET_POLY_*/SET_SOURCEPOLY_*/SET_HANDLEPOLY_*:
    // the 0x80-byte polygon records of the world).
    std::optional<std::size_t> h_world_poly(VmState&, const VmInsn&);
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
