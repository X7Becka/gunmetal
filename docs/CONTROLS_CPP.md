# `input/controls.h` — `.key` profiles and controller config

Status: **read-only, proven against the 1.36 supply**. Implementation:
`engine/src/input/controls.cpp`; tests: `engine/tests/test_controls.cpp`.
Formats: `docs/CONFIG.md` §6 and §9; the tables below come from `gundx.exe`
1.36 itself, not from guesswork. No SDL and no global state: the caller feeds
raw key/button/axis values, `Controls::update()` does the rest.

---

## 1. What is parsed

### 1.1 `KEYCFG/*.KEY`

```
0x000  char name[16]                 // profile name, NUL padded
0x010  45 * { u32 a; u32 b }         // 0x168 bytes
       total 0x178 = 376 bytes
```

The loader (`0x4852e7`…`0x4853e6`) does `fread(buf, 0x178, 1, f)`, copies the
name with `strncpy(..., 16)` and unpacks **31 pairs** into the working record
`name[16]` at +0xF8 (`0x6c2058 + i*0x108 + 0xF8`, writer at `0x485440`).

* `a[31]` — the key code per action, `0` = unbound. This is what `Controls`
  turns into bindings.
* `b[31]` — a second code table, **all zero in every shipped file**. Its meaning
  is not proven, so it is kept in `KeyProfile::pairs` and not interpreted.
* Pairs 31..44 (0x108…0x177) are the writer's uninitialised stack tail. All
  three shipped files carry the same 14 pairs; they are preserved verbatim and
  never treated as actions.

Unknown but non-negative codes (123…0x7FFFFFFF) are **preserved**:
`profile().pairs[i].a`, `unknown_keys()`, and `key_name()` → `KEY_<n>`. A code
with the top bit set is rejected as corrupt (`bad key code 0x...`) instead of
silently turning into a negative index.

### 1.2 `GUNMETAL.CFG` controller fields

Magic: `"GUNMETAL Configuration File. av1.93\x1a\0"`, all 0x25 bytes compared
including the final NUL. Exactly 0x2000 bytes are used; the whole image is kept
in `ConfigProfile::raw`.

| offset | size | field |
|---|---|---|
| 0x1E0 + 8k | 9×u32 | profile name 1 (low byte of each dword) |
| 0x1E4 + 8k | 9×u32 | profile name 2, interleaved with name 1 |
| 0x248 | u32 | `joystick_used_by` (1 = Player 1) |
| 0x24C | 3×u32 | axis presets: left/right, forward/back, up/down |
| 0x258 | 3×u32 | sensitivity in **milliseconds** (2200 in the supply) |
| 0x264 | 4×u32 | joystick button → action index (5, 15, 5, 15) |
| 0x27C | 4×0x18C | controller records |

Each 0x18C record: `slot` (+0x000), `buttons[32]` (+0x004), six
`{mask_a, mask_b, preset}` axes (+0x084), sixteen `{bit, lo, hi}` entries
(+0x0CC). The four shipped records are identical.

`sensitivity_seconds()` reproduces the loader's proven floor:
`max(0.2, ms / 1000)` (`0x485272`). It is **not** a deadzone.

Profile names select the key file; `Controls` does not auto-search `KEYCFG`:
load the `PERFECT`/`SPLIT1` file explicitly after reading the config.

### 1.3 Actions (31 slots)

The pointer table at RVA `0xBC558` has exactly 31 strings, in `.key` slot order.
`Controls::action_name(slot)` is that table in snake_case; `action_slot()` also
accepts a few aliases (`backward`, `previous_weapon`, `previous_equipment`,
`toggle_equip`, `equip_cycle`, `strafe_l`, `strafe_r`, `throttle`).

| slot | action | PERFECT | SPLIT1 | SPLIT2 |
|---:|---|---|---|---|
| 0 | `forward` | W (0x11) | U (0x16) | NUM8 (0x48) |
| 1 | `back` | S (0x1F) | M (0x32) | NUM2 (0x50) |
| 2 | `turn_left` | LEFT (0x6F) | H (0x23) | NUM4 (0x4B) |
| 3 | `turn_right` | RIGHT (0x70) | K (0x25) | NUM6 (0x4D) |
| 4 | `look_center` | C (0x2E) | J (0x24) | NUM5 (0x4C) |
| 5 | `fire` | LCTRL (0x1D) | LCTRL (0x1D) | NUM0 (0x52) |
| 6 | `look_up` | UP (0x6D) | I (0x17) | NUM9 (0x49) |
| 7 | `look_down` | DOWN (0x72) | COMMA (0x33) | NUM3 (0x51) |
| 8 | `strafe` | LALT (0x38) | LALT (0x38) | RALT (0x6B) |
| 9 | `strafe_left` | A (0x1E) | V (0x2F) | LEFT (0x6F) |
| 10 | `strafe_right` | D (0x20) | B (0x30) | RIGHT (0x70) |
| 11 | `throttle_up` | E (0x12) | A (0x1E) | NPLUS (0x4E) |
| 12 | `throttle_down` | Q (0x10) | Z (0x2C) | NMNUS (0x4A) |
| 13 | `next_weapon` | RSQB (0x1B) | LSFT (0x2A) | RSFT (0x36) |
| 14 | `prev_weapon` | LSQB (0x1A) | CAPS (0x3A) | ENTER (0x1C) |
| 15 | `activate` | SPACE (0x39) | SPACE (0x39) | NUM1 (0x4F) |
| 16 | `next_equipment` | QUOTE (0x28) | TAB (0x0F) | RSQB (0x1B) |
| 17 | `prev_equipment` | SEMI (0x27) | TILDA (0x29) | LSQB (0x1A) |
| 18 | `toggle_equipment` | ENTER (0x1C) | Q (0x10) | NENTR (0x66) |
| 19 | `headlights` | F (0x21) | — | — |
| 20 | `force_field` | G (0x22) | — | — |
| 21 | `mine_detector` | N (0x31) | — | — |
| 22 | `targeting` | T (0x14) | — | — |
| 23 | `warp_device` | V (0x2F) | — | — |
| 24 | `radar` | R (0x13) | — | — |
| 25 | `mine_layer` | M (0x32) | — | — |
| 26 | `combat_block` | B (0x30) | — | — |
| 27 | `auto_sentry` | X (0x2D) | — | — |
| 28 | `decoy` | Z (0x2C) | — | — |
| 29 | `linear_left` | — | — | — |
| 30 | `linear_right` | — | — | — |
| 31..44 | stack tail, same 14 pairs in all three files | | | |

### 1.4 Key codes are the game's own table, not DIK

The name table is at RVA `0x4A5B88`, **123 entries × 6 bytes**, indexed directly
by the `.key` code. Codes 0x01…0x53 look like scancodes, but the upper range is
custom: `UP=0x6D`, `LEFT=0x6F`, `RIGHT=0x70`, `DOWN=0x72`, `F11=0x54`,
`F12=0x55`. This is why `PERFECT` stores 0x6D/0x6F/0x70/0x72 for look/turn.
`Controls::key_name()` returns the exact 6-byte label (`W`, `LCTRL`, `NUM0`,
`PG UP`, …); unknown codes become `KEY_<n>`.

Notable values used by the goldens: `W=0x11`, `S=0x1F`, `SPACE=0x39`,
`LCTRL=0x1D`, `NUM0=0x52`, `NPLUS=0x4E`.

---

## 2. API

```cpp
struct Binding {
    std::string action;
    int key = -1;    // one of key/axis/button is >= 0
    int axis = -1;
    int button = -1;
    bool invert = false; // flips an axis value
};

enum class ConflictPolicy { reject, allow };

struct ActionState {
    double value;      // [-1, 1]; digital sources are 0 or 1
    bool pressed;      // rising edge this update
    bool held;         // |value| >= 0.5 (1e-9 tolerance)
    bool released;     // falling edge this update
    double held_time;  // seconds, accumulated with dt
};

class Controls {
    Controls();                                  // shipped PERFECT, dz 0.2
    bool load_key_file(path, std::string* error = nullptr);
    bool load_config(path, std::string* error = nullptr);
    bool bind(const Binding&, ConflictPolicy = reject, std::string* error = nullptr);

    void set_key(int code, bool down);
    void set_button(int index, bool down);
    void set_axis(int index, double value);

    bool update(double dt, std::string* error = nullptr);

    std::optional<std::string> map_key(int code) const;
    std::optional<std::string> map_button(int index) const;
    std::optional<AxisSample> map_axis(int index, double raw) const;

    const ActionState& action(std::string_view) const;
    bool pressed/held/released(std::string_view) const;
    double value(std::string_view) const;
    Vec2 move_vector() const;  // forward/strafe, length <= 1
    Vec2 look_vector() const;  // turn+linear / look, length <= 1

    bool set_deadzone(double, std::string* error = nullptr);
    const KeyProfile& profile() const;
    const ConfigProfile& config() const;
    const std::vector<Binding>& bindings() const;
    const std::vector<KeyLocation>& unknown_keys() const;
    std::vector<std::string> conflicts() const;
};
```

Defaults: `PERFECT` (slots 0..28 bound, 29/30 unbound) and deadzone **0.2**.
The deadzone is application policy — `GUNMETAL.CFG` has no deadzone field; its
`0.2` is a sensitivity *floor in seconds*.

### 2.1 Bind policy

* Exactly one of `key`/`axis`/`button` must be non-negative; `key == 0`
  (`NONE`) is rejected; unknown action names are rejected.
* A **key** binding replaces the action's existing keyboard binding (one key per
  action, like the `.key` file). A **button** binding replaces only the same
  button index, so button 0 and button 2 can both fire. An **axis** binding
  replaces only the same axis *and* `invert`, so `turn_left(-)` and
  `turn_right(+)` coexist on axis 0.
* `ConflictPolicy::reject` (default) refuses a key/button already owned by
  another action, or the same axis sign owned by another action, with e.g.
  `controls: key 17 (W) already bound to 'forward'`. `allow` adds it and
  `conflicts()` reports `key 17 (W): forward/fire`.
* Shipped profiles are loaded as-is: if a file itself repeats a key, the first
  binding in slot order wins in `map_key()` and `conflicts()` shows the repeat.
  Loading never fails because of a duplicate.

### 2.2 Update, deadzone, normalisation

`update(dt)` validates `dt` (finite, `>= 0`) and every raw axis (finite) before
touching state; on error it returns `false` with a message and leaves the
previous `ActionState`s intact.

Per action: sum the pressed keys/buttons (+1 each) and every mapped axis, clamp
to `[-1, 1]`, then compute `pressed/held/released` against the previous tick and
accumulate `held_time`. Axes use:

```
v = clamp(raw, -1, 1)
if |v| <= deadzone: 0
else: sign(v) * (|v| - deadzone) / (1 - deadzone)
```

`invert` flips `raw` before the deadzone. `deadzone == 1` is accepted and
silences every axis (no division by zero); values outside `[0, 1]` and NaN are
rejected. A `0.5` that lands on `0.49999999999999994` still counts as held via a
1e-9 tolerance.

`move_vector()` is `{forward - back, strafe_right - strafe_left}`;
`look_vector()` is `{turn_right - turn_left + linear_right - linear_left,
look_down - look_up}`. If the length exceeds 1 both components are divided by
it, so a digital W+D diagonal is exactly `(√½, √½)` with length 1.

### 2.3 Config bindings

`load_config()` keeps existing keys and adds:

* button `i` (0..3) → action `buttons[i]` (5 Fire, 15 Activate in the supply);
* axis 0..2 → the named preset pair, e.g. preset 4 `Linear Turn L/R` becomes
  `turn_left` (inverted) + `turn_right` on that axis; preset 8 `Look Up/Down`
  becomes `look_up` + `look_down`.

The preset→action pair comes from the 18-entry name table at `0x4BC468`; the
sign convention (L/Up = negative) is inferred from the names (see §5).

---

## 3. Goldens and counts

Summary of the three shipped files:

| file | name | bound slots | pairs | `b` | bytes |
|---|---|---:|---:|---|---:|
| `KEYCFG/PERFECT.key` | PERFECT | **29** | 45 | all 0 | 376 |
| `KEYCFG/SPLIT1.KEY` | SPLIT1 | **19** | 45 | all 0 | 376 |
| `KEYCFG/SPLIT2.KEY` | SPLIT2 | **19** | 45 | all 0 | 376 |

Every code is pinned in the table of §1.3 and in `test_controls.cpp`. The
14-pair tail is identical in all three files:

```
pairs[31] = (4084748653, 0)
pairs[32..42] = (0, 0)                  // 11 pairs
pairs[43] = (2703360, 0)
pairs[44] = (1761392, 2000535658)
```

`GUNMETAL.CFG` goldens: names `PERFECT`/`SPLIT1`; `joystick_used_by=1`;
presets `4, 8, 0`; sensitivity `2200, 2200, 2200` ms → `2.2, 2.2, 2.2` s;
buttons `5, 15, 5, 15`; four identical records with `buttons[0]=0x20`,
`buttons[1]=0x8000`, `buttons[2..31]=0xFFFFFFFF`, axes
`(4,8,0) (1,2,0) (0x800,0x800,0) (-1,-1,0)*3`, bits
`6,13,7,14` then `(-1,0,0)*12`.

---

## 4. Build and test

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DGM_BUILD_TESTS=ON -DGM_WITH_SDL3=ON \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox --target gm_controls_tests -j 32
ctest --test-dir build/sandbox -R controls --output-on-failure
python3 regress.py
```

`controls` carries the **quick** label (≈0.04 s). The real-file half reads the
game install read-only and prints `SKIP` when `KEYCFG/PERFECT.key` is absent;
`GUNMETAL_DIR` overrides `/mnt/d/games/Gunmetal 3dfx`. Synthetic fixtures use
`$TMPDIR` only.

Coverage: action/key tables, defaults, bind/override, conflict reject+allow,
axis-pair exceptions, edges (`pressed`/`held`/`released`, `held_time`),
multi-source actions, deadzone boundaries, invert, deadzone 1, diagonal length,
unknown keys, truncated/bad-key/empty-name/no-file `.key`, trailing bytes,
real three profiles (every code, counts, tail), config fields/records applied
to state, truncated and bad-magic config, negative `dt`, NaN axis (state kept),
`deadzone > 1`.

---

## 5. Trust: proven, data, hypothesis

**Proven by code (`gundx.exe` 1.36):**

* `.key`: 0x178 bytes, `name[16]` + 45 pairs, loader uses 31, name at +0xF8 of
  the working record (`0x4852e7`…, `0x4853e6`, writer `0x485440`).
* 31-entry action pointer table at RVA `0xBC558`, in slot order.
* 123-entry key-name table at RVA `0x4A5B88`, 6 bytes per entry; this is the
  code space of `.key` (hence `UP=0x6D`, `LEFT=0x6F`).
* `GUNMETAL.CFG` size/magic/field offsets, interleaved 9-byte names,
  milliseconds → seconds with the `0.2` s floor.
* Button → action table at RVA `0xBC548`: index 5 Fire, 15 Activate, matching
  the config's `5, 15`.
* Axis preset names (18 records) at RVA `0x4BC468`.

**Proven by data (supply):** counts 29/19/19, every code in §1.3, `b=0`,
identical stack tail, config values and four identical records, the two config
profile names existing as `PERFECT.key`/`SPLIT1.KEY`.

**Hypothesis / deliberately not guessed:**

* `b[31]` is a second key/device table; zero in the supply, meaning unproven.
* Physical meaning of the 32 channel masks and the four controller records;
  they are exposed verbatim but not used by `update()`.
* Preset sign convention (L/Up negative) and the exact action pair for
  `Weapons ±` / `Equip Cycle ±` (derived from the names).
* Unknown-code policy, deadzone default 0.2, conflict policy and diagonal
  normalisation are application choices, not game behaviour.

---

## 6. Remaining

* No SDL event pump; a platform layer must call `set_key/set_button/set_axis`
  and `update(dt)`. Keycodes are the game's, so a Windows/DOS scancode bridge
  still needs the 123-entry translation (the array is in `controls.cpp`).
* `b[31]` is preserved but not bound; controller records and the 32 channel
  masks are parsed but not interpreted.
* Sensitivity is parsed (and floored) but not applied to look/turn; the caller
  decides how to scale `look_vector()`.
* Read-only: no writer for `.key`/`GUNMETAL.CFG`, no profile switching UI.
* No key repeat/text input, no gamepad hot-plug, no per-player split-screen
  routing (four records are exposed, not assigned).
