# C++ reader fuzzing (`engine/tests/test_fuzz.cpp`)

The Python toolkit has `tools/fuzz_formats.py`; this is its C++ twin. It runs
in-process against the real reader API, so trust-boundary problems that a
golden-corpus test cannot see (crashes, hangs, signed overflow, gigantic
allocations from a 4-byte header) show up as a failed test instead of a
production surprise.

## What is covered

| Format | API exercised | Seeds |
|---|---|---|
| BIGFILE `.idx/.dat` | `gm::BigFile::open`, `BigFile::read` | synthetic valid pair in a temp dir; 10 DAT sizes from 0 to 4096; forged out-of-range `BigEntry`s |
| SBV container | `gm::parse_sbv`, `gm::sbv_offsets_valid` | synthetic 320x160 clip + a compact (first 8 frames) `Vid/Intro.sbv` |
| SBV codec | `gm::decode_sbv_frame` | first frame block of Intro, synthetic zero blocks, width/height matrix, frame-header dwords |
| SBW | `gm::parse_sbw` (and therefore `parse_sbw_header`) | synthetic empty-table level + `level0.sbw` |
| SBT | `gm::parse_sbt` | synthetic header+terminator + `LEVEL0.SBT` |
| SBM | `gm::parse_sbm` | synthetic 4x4 mip + first texture from `PICS/SBM.IDX` |
| SBC | `gm::parse_sbc` | synthetic one-record block + `DM2.sbc` |
| SOB | `gm::parse_sob` | synthetic two-face stream + `SOBS/20mmclp.sob` |
| SOF | `gm::parse_sof`, `Sof::frame` | synthetic two-tile film |
| SOU1 | `gm::decode_sou1_raw` | synthetic 8 kHz mono blob (`0x4b` payload) |

Every seed gets:

* the empty input and **every** prefix of length `0..256`;
* additional short prefixes (320/512/1024, `n/2`, `n-1`) and random cuts;
* one-byte header substitutions at offsets `0..63` with `00/7F/80/FF`;
* random single-bit flips over the whole blob;
* `0`, `1`, `INT_MAX`, `INT_MIN`, `UINT_MAX` in each dword of the first
  32 bytes;
* SBV-only: width/height/count/end-offset matrices and geometry pairs from
  `0`/negative values up to `0xFFFFFFFF`, plus payload mutations routed through
  the pixel decoder.

`SOU2`, `SOU0` (raw PCM), BMP/PNG, `.soe` and the VM are not covered here.
SOU2 has its own full-corpus parity test (`test_sou2.cpp`), and `.sof`/SOU1
have golden tests; the remaining formats are out of scope for this matrix.

The SOB coverage already paid for itself: the wide run found a truncated
face-index list read past the token vector and an unbounded face-chain
recursion.  `parse_sob` now checks `q + nv <= tokens.size()` and caps the
recursive sentinel walk at 4096 faces (shipped maximum is 1130).

## Running

```sh
export PATH=$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH
cmake -S . -B build/sandbox -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=$HOME/tools/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/tools/bin/zigcxx \
      -DSDL3_DIR=$HOME/tools/sdl3/lib/cmake/SDL3
cmake --build build/sandbox -j

./build/sandbox/engine/tests/gm_fuzz_tests            # same as --quick
./build/sandbox/engine/tests/gm_fuzz_tests --full     # wider budgets

ctest --test-dir build/sandbox -R fuzz --output-on-failure
ctest --test-dir build/sandbox -L quick                # includes `fuzz`
ctest --test-dir build/sandbox -L full                 # includes `fuzz_full`
```

`fuzz` is labelled `quick` and runs `--quick`; `fuzz_full` is labelled `full`.
The shipped game directory is an optional seed source — without it the
synthetic seeds still cover every entry point above.

## Limits

* **Per-case wall clock:** measured with `std::chrono` *after* the call
  returns; a case over 1000 ms is a failure. This cannot interrupt an infinite
  loop, so the CTest `TIMEOUT` (300 s quick / 900 s full) is the watchdog.
  A hang therefore fails the suite, but only after the CTest timeout.
* **Case cap:** 8 000 cases for `--quick`, 30 000 for `--full`; reaching the cap
  is reported (`[case cap reached]`) so a runaway generator is visible.
* **Allocation cap:** the test binary replaces global `operator new`/`new[]`;
  any single allocation above **64 MiB** is recorded as a failure and turned
  into `std::bad_alloc`. This is what catches a header counter like
  `0xFFFFFFFF` that would otherwise reserve gigabytes. The replacement is
  checked by a self-test at startup.
* **Seed cap:** real files are read read-only and truncated to 8 MiB before
  mutation (every shipped seed is smaller).
* Expected outcomes are: successful parse, `std::nullopt`, `false`, or a
  standard exception. A non-standard exception, `std::bad_alloc`, an oversized
  allocation, a slow case or a crash is a failure.

Measured on this machine (Debug sandbox build / RelWithDebInfo):

| Mode | Debug | Release |
|---|---|---|
| `--quick` | ~5.7 s (4 396 cases) | ~0.3 s |
| `--full` | ~23 s (10 399 cases) | ~0.8 s |

The ASan+UBSan preset (`build/asan`) also runs both modes clean.

## Bugs found by this matrix and fixed

1. **SBV codec: infinite loop for small geometry** (`engine/src/res/sbv_video.cpp`).
   `decode_sbv_frame` accepted `width >= 8, width even, height % 16 == 0`, but
   `lift_2d` decrements its stride counter by 4 and requires a stride divisible
   by 4. With `width/8 == 1` (width 8..15) or `height/8 == 2` (height 16) its
   loop never reached zero and never returned. Regression: the geometry matrix
   plus explicit 8x16/10x16/14x16/… cases; the guard now requires
   `width % 16 == 0 && height % 16 == 0 && height >= 32` (shipped clips are
   320x160).
2. **SBV codec: signed overflow in `base_grid`** (`sbv_video.cpp`).
   `hi - lo` was evaluated in `int32_t`; a frame block starting with
   `INT_MAX, INT_MIN` is UB and traps in the checked Debug build. It is now
   computed in `double`.
3. **SBM: negative shift in the mip loop** (`engine/src/res/sbm.cpp`).
   `1 << (log2w - k)` was UB when `levels_minus_one > log2w` (or `log2h`).
   Such files are now rejected before the loop, matching `gm/sbm.py`
   (`expected_size` raises for the same input).
4. **SBW: unbounded header reserve** (`engine/src/res/sbw.cpp`).
   A 4-byte blob `FF FF FF FF` made `textures.reserve(0xFFFFFFFF)` ask for
   ~160 GiB (measured `std::bad_alloc`). Texture/effect/sector counts are now
   bounded by the smallest possible table entry in the remaining input before
   `reserve`; the polygon `reserve` is bounded by the 124-byte minimum polygon
   size as well.

Each fix has an explicit regression case in `engine/tests/test_fuzz.cpp`
(besides the generated mutations that found it).

## Residual risks

* No in-process watchdog: a new infinite loop is only killed by the CTest
  timeout (300/900 s). Running the binary by hand needs `timeout`.
* Mutation is single-bit / single-byte / single-dword; there is no coverage
  guidance or libFuzzer corpus minimisation.
* `BigFile` is only fed small synthetic indexes; a DAT larger than 4 GiB (the
  32-bit offset limit) is not materialised in the test.
* Seeds read from `/mnt/d/games/Gunmetal 3dfx` are not part of the repository;
  coverage on a machine without the game is shallower, not zero.
