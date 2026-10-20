"""Independent model of the ``SOU1`` (4-bit Miles ADPCM) decoder.

This is the *static* counterpart of :mod:`tools.re.sou_decode`: instead of
single-stepping ``gundx.exe`` it reimplements the three decoders
``0x852b40`` / ``0x852c00`` / ``0x852cc0`` directly, and a ``--verify`` mode
proves it against the real bytes of the executable on the whole shipped
corpus (983 ``SOU1`` entries in ``sounds/SOU.IDX``).

The codec
---------

The three routines are the same loop with 1, 2 or 4 copies of every output
sample (the mixer runs at 32 kHz, the samples at 32/16/8 kHz):

    for every payload byte (two nibbles, high first):
        code  = b >> 4, then b & 15
        delta = table[level + code*4]      ; 32-bit signed
        acc   = (acc + delta) & 0xffffffff ; 32-bit accumulator
        level = (level + step[code]) & 0xffffffff
        emit acc, ``copies`` times

* ``step`` lives at ``0x850500`` and is ``-64`` for codes 0..3 / 8..11,
  ``128/256/384/512`` for codes 4..7 / 12..15.
* ``table`` is a pointer into **one shared 0x1d80-byte delta curve** that
  starts at ``0x850540`` and ends at ``0x8522c0`` (the next bytes are the
  *SOU2* step table).  ``[0x850480 + (vol&31)*4]`` selects a starting row;
  the index is *not* clamped -- the adaptation level walks inside the shared
  curve.  The volume index is the game's per-channel volume minus one
  (0..31, 31 = loudest), see ``0x40a4b0``/``0x40a690``.
* the decoder **adds** its output to the destination buffer (``add [edi],ebx``
  in the real code); a standalone decode therefore needs a zeroed buffer.
* ``acc``/``level`` are a per-voice, per-channel 8-byte state
  ``{acc, level}``.  The mixer passes ``[[voice]+0x40]`` for the left and
  ``[[voice]+0x48]`` for the right channel.  The voice-start routine
  ``0x40a5e0`` zeroes all four dwords (``0x40a64d..0x40a65f``), so a fresh
  sample always starts from ``acc = level = 0``.

Stereo payloads are **nibble-interleaved**: two consecutive payload bytes
``A B`` hold the high nibbles of a left byte and the low nibbles of a right
byte.  The loader ``0x40b2d8`` de-interleaves them into two contiguous
half-buffers (``left = (A & 0xf0) | (B >> 4)``, ``right = ((A << 4) & 0xf0) | (B & 15)``)
of ``length/2`` bytes each, decoded independently with their own state.
Mono entries have ``flags == 0``; six entries are stereo (``flags == 1``).

Usage::

    python3 tools/re/sou1_model.py --list
    python3 tools/re/sou1_model.py SILENCE out.wav
    python3 tools/re/sou1_model.py --verify            # all 983, slow
    python3 tools/re/sou1_model.py --verify --limit 40 # quick subset
    python3 tools/re/sou1_model.py --verify --json out/sou1.json
    python3 tools/re/sou1_model.py --check docs/manifests/sou1-fnv.json
"""

from __future__ import annotations

import argparse
import array
import json
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

import exelib  # noqa: E402
from gm import bigfile  # noqa: E402

RATE_OUT = 32000  # the mixer runs at 32 kHz

STEP_TABLE_VA = 0x850500
VOLUME_PTRS_VA = 0x850480
VOLUME_STATE_VA = 0x84A6A8
DELTA_BASE = 0x850540
DELTA_END = 0x8522C0  # exclusive; 0x8522c0 is the SOU2 step table


class Sou1Error(ValueError):
    pass


def load_tables() -> tuple[list[int], list[int], list[int]]:
    """``(step[16], volume_ptr[32], delta_curve)`` read from ``gundx.exe``.

    ``delta_curve[k]`` is the signed dword at ``DELTA_BASE + 4*k``.
    """
    data = exelib.data()

    def s32(va: int) -> int:
        off = exelib.va2off(va)
        return struct.unpack_from("<i", data, off)[0]

    def u32(va: int) -> int:
        off = exelib.va2off(va)
        return struct.unpack_from("<I", data, off)[0]

    step = [s32(STEP_TABLE_VA + 4 * i) for i in range(16)]
    pointers = [u32(VOLUME_PTRS_VA + 4 * i) for i in range(32)]
    curve = [s32(DELTA_BASE + 4 * i) for i in range((DELTA_END - DELTA_BASE) // 4)]
    return step, pointers, curve


STEP, VOLUME_PTRS, CURVE = load_tables()


def factor_for(rate: int) -> int:
    """Output slots per payload byte: 8 / 4 / 2 for 8 / 16 / 32 kHz."""
    if rate < 10000:
        return 8
    if rate < 18000:
        return 4
    return 2


def decoder_address(rate: int) -> int:
    """The routine the loader picks for ``rate`` (``0x40b246``)."""
    return {8: 0x852CC0, 4: 0x852C00, 2: 0x852B40}[factor_for(rate)]


def decode_stream(payload: bytes, rate: int, volume: int = 31) -> array.array:
    """Decode one mono channel to 32-bit accumulator slots (unsigned)."""
    copies = factor_for(rate) // 2
    table_ptr = VOLUME_PTRS[volume & 31]
    base = (table_ptr - DELTA_BASE) // 4
    if table_ptr < DELTA_BASE or table_ptr >= DELTA_END:
        raise Sou1Error("volume table pointer %#x outside the delta curve" % table_ptr)

    step = STEP
    curve = CURVE
    # ``level`` is a byte offset; with ``base`` already pointing at the chosen
    # volume row, the last readable dword is ``curve[-1]``.
    max_level = (len(curve) - base - 16) * 4
    out = array.array("I")
    append = out.append
    extend = out.extend
    acc = 0
    level = 0
    for byte in payload:
        # two nibbles, high first -- exactly the two additions in the loop
        if level > max_level:
            raise Sou1Error("adaptation level %#x leaves the delta curve" % level)
        w = base + (level >> 2)
        acc = (acc + curve[w + (byte >> 4)]) & 0xFFFFFFFF
        append(acc)
        if copies > 1:
            extend([acc] * (copies - 1))
        level = (level + step[byte >> 4]) & 0xFFFFFFFF

        if level > max_level:
            raise Sou1Error("adaptation level %#x leaves the delta curve" % level)
        w = base + (level >> 2)
        acc = (acc + curve[w + (byte & 15)]) & 0xFFFFFFFF
        append(acc)
        if copies > 1:
            extend([acc] * (copies - 1))
        level = (level + step[byte & 15]) & 0xFFFFFFFF

    return out


def deinterleave_stereo(payload: bytes) -> tuple[bytes, bytes]:
    """Split a nibble-interleaved SOU1 payload (loader ``0x40b2d8``)."""
    n = len(payload) & ~1
    left = bytearray(n // 2)
    right = bytearray(n // 2)
    for i in range(0, n, 2):
        a = payload[i]
        b = payload[i + 1]
        left[i // 2] = (a & 0xF0) | (b >> 4)
        right[i // 2] = ((a << 4) & 0xF0) | (b & 0x0F)
    return bytes(left), bytes(right)


def parse_entry(data: bytes) -> tuple[int, int, bytes]:
    """``(rate, flags, effective payload)`` for a ``SOU1`` blob."""
    if data[:4] != b"SOU1":
        raise Sou1Error("not a SOU1 entry: %r" % data[:4])
    rate, flags = struct.unpack_from("<II", data, 4)
    length = struct.unpack_from("<I", data, 12)[0]
    payload = data[16:16 + length]
    return rate, flags, payload[:len(payload) & ~1]


def decode_entry(data: bytes, volume: int = 31):
    """Decode a whole entry.

    Returns ``(rate, channels, slots_per_channel, streams)`` where ``streams``
    is a list of ``array('I')`` (one per channel).  ``channels`` is 1 or 2.
    """
    rate, flags, payload = parse_entry(data)
    if flags & 1:
        left, right = deinterleave_stereo(payload)
        streams = [decode_stream(left, rate, volume),
                   decode_stream(right, rate, volume)]
        channels = 2
    else:
        streams = [decode_stream(payload, rate, volume)]
        channels = 1
    slots = len(streams[0])
    for s in streams:
        if len(s) != slots:
            raise Sou1Error("channel length mismatch")
    return rate, channels, slots, streams


def read_entry(name: str, game: str) -> bytes:
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    return bf.read(bf[name])


def list_entries(game: str) -> list[str]:
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    return [e.name for e in bf.entries if bf.read(e)[:4] == b"SOU1"]


# ---------------------------------------------------------------------------
# verification against the real decoder bytes
# ---------------------------------------------------------------------------

# Memory layout for the emulator run.  The decoder *adds* into the destination
# and the state lives in memory, so both areas must be separate and zeroed per
# chunk.  The old harness placed the output at 0x300000 and the state at
# 0x400000: output slot 0x40000 (262144) lands exactly on the state dwords,
# which is what made "long" tracks diverge.
EMU_PAYLOAD = 0x100000
EMU_OUTPUT = 0x300000
EMU_STATE = 0x0E0000
EMU_CHUNK = 0x10000  # slots; a multiple of every factor (2/4/8)


def _emu_cpu():
    import x86mini

    cpu = x86mini.CPU()
    image = exelib.data()
    for _name, va, rawsize, off in exelib.sections():
        cpu.load(va, image[off:off + rawsize])
    return cpu


def decode_stream_emu(payload: bytes, rate: int, volume: int = 31,
                      chunk: int = EMU_CHUNK) -> bytes:
    """Run the shipped decoder bytes on ``payload``; returns raw int32 LE."""
    import x86mini

    factor = factor_for(rate)
    total = len(payload) * factor
    func = decoder_address(rate)
    if EMU_STATE + 8 > EMU_PAYLOAD or EMU_PAYLOAD + len(payload) > EMU_OUTPUT:
        raise Sou1Error("emulator layout overflow: payload=%d" % len(payload))
    if EMU_OUTPUT + chunk * 4 > 0x401000:  # below the loaded .text image
        raise Sou1Error("emulator output chunk reaches the exe image")
    cpu = _emu_cpu()
    cpu.wr8(VOLUME_STATE_VA, volume)
    cpu.load(EMU_PAYLOAD, payload)
    cpu.wr32(EMU_STATE, 0)
    cpu.wr32(EMU_STATE + 4, 0)
    out = bytearray()
    done = 0
    while done < total:
        n = min(chunk, total - done)
        src = EMU_PAYLOAD + done // factor
        cpu.mem[EMU_OUTPUT:EMU_OUTPUT + n * 4] = bytes(n * 4)
        cpu.call(func, [src, EMU_OUTPUT, n, EMU_STATE],
                 limit=n * 40 + 100000)
        out += cpu.read(EMU_OUTPUT, n * 4)
        done += n
    return bytes(out)


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


PCM16_SHIFT = 5  # observed codec full scale is 2^20 -> 2^15


def pcm16(raw_i32: bytes) -> bytes:
    """16-bit presentation of the accumulator stream.

    The codec's accumulator tops out at about 2^20 on the shipped corpus
    (``docs/SOU1.md``), so >>5 maps full scale to int16.  The game's mixer
    applies its own gain later; the raw int32 stream stays the reference.
    """
    out = bytearray(len(raw_i32) // 2)
    for i, (s,) in enumerate(struct.iter_unpack("<i", raw_i32)):
        v = s >> PCM16_SHIFT
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        struct.pack_into("<h", out, 2 * i, v)
    return bytes(out)


def _payload_streams(data: bytes) -> tuple[int, int, bytes, list[bytes]]:
    rate, flags, payload = parse_entry(data)
    if flags & 1:
        left, right = deinterleave_stereo(payload)
        streams = [left, right]
    else:
        streams = [payload]
    return rate, 1 + (flags & 1), payload, streams


def verify(game: str, limit: int | None = None, json_path: str | None = None,
           chunk: int = EMU_CHUNK, verbose: bool = True) -> int:
    names = sorted(list_entries(game))
    if limit is not None:
        names = names[:limit]
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    records = []
    bad = 0
    total_slots = 0
    for idx, name in enumerate(names):
        data = bf.read(bf[name])
        rate, channels, payload, streams = _payload_streams(data)
        slots = len(streams[0]) * factor_for(rate)
        total_slots += slots
        fstreams = []
        emu_arrays = []
        ok = True
        first_bad = None
        for ch, stream in enumerate(streams):
            model = decode_stream(stream, rate)
            emu = decode_stream_emu(stream, rate, chunk=chunk)
            emu_arr = array.array("I")
            emu_arr.frombytes(emu)
            if bytes(emu) != model.tobytes():
                ok = False
                if first_bad is None:
                    for i, (x, y) in enumerate(zip(emu_arr, model)):
                        if x != y:
                            first_bad = (ch, i, x, y)
                            break
            fstreams.append(fnv1a64(emu))
            emu_arrays.append(emu_arr)
        if channels == 2:
            interleaved = array.array("I")
            left, right = emu_arrays
            for i in range(slots):
                interleaved.append(left[i])
                interleaved.append(right[i])
        else:
            interleaved = emu_arrays[0]
        raw = interleaved.tobytes()
        rec = {
            "name": name,
            "rate": rate,
            "channels": channels,
            "payload_bytes": len(payload),
            "slots_per_channel": slots,
            "seconds": round(slots / RATE_OUT, 6),
            "fnv1a64": "%016x" % fnv1a64(raw),
            "fnv1a64_pcm16": "%016x" % fnv1a64(pcm16(raw)),
            "fnv1a64_channels": ["%016x" % h for h in fstreams],
        }
        records.append(rec)
        if not ok:
            bad += 1
            print("MISMATCH %s: first %r" % (name, first_bad))
        if verbose and (idx + 1) % 50 == 0:
            print("  %d/%d ..." % (idx + 1, len(names)), flush=True)

    manifest = {
        "codec": "SOU1 4-bit Miles ADPCM",
        "corpus": "sounds/SOU.IDX",
        "game_version": "Gunmetal 3dfx 1.36",
        "records": len(records),
        "total_slots": total_slots,
        "total_seconds": round(total_slots / RATE_OUT, 6),
        "exact": len(records) - bad,
        "mismatched": bad,
        "chunk_slots": chunk,
        "pcm16_shift": PCM16_SHIFT,
        "entries": records,
    }
    if json_path:
        os.makedirs(os.path.dirname(os.path.abspath(json_path)), exist_ok=True)
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, indent=1)
            fh.write("\n")
    print("verified %d/%d SOU1 entries bit-exact against gundx.exe "
          "(%d slots, %.2f s of audio)"
          % (len(records) - bad, len(records), total_slots,
             total_slots / RATE_OUT))
    return 1 if bad else 0


def _model_pcm(rate: int, channels: int, payload_streams) -> tuple[array.array, int]:
    """Decode channels and build the interleaved PCM32 stream."""
    decoded = [decode_stream(p, rate) for p in payload_streams]
    slots = len(decoded[0])
    if channels == 2:
        left, right = decoded
        interleaved = array.array("I")
        for i in range(slots):
            interleaved.append(left[i])
            interleaved.append(right[i])
    else:
        interleaved = decoded[0]
    return interleaved, slots


def check_manifest(game: str, path: str) -> int:
    """Model-only regression check against a committed FNV manifest.

    This does not run the x86 emulator, so it is fast; it proves the static
    model still produces the same PCM (FNV-1a64, lengths, durations) that was
    verified bit-exactly against ``gundx.exe``.
    """
    with open(path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    expected = {e["name"]: e for e in manifest["entries"]}
    bf = bigfile.BigFile(os.path.join(game, "sounds", "SOU.IDX"))
    bad = 0
    checked = 0
    for name in sorted(expected):
        data = bf.read(bf[name])
        rate, channels, payload, streams = _payload_streams(data)
        pcm, slots = _model_pcm(rate, channels, streams)
        raw = pcm.tobytes()
        want = expected[name]
        got = {
            "rate": rate,
            "channels": channels,
            "payload_bytes": len(payload),
            "slots_per_channel": slots,
            "seconds": round(slots / RATE_OUT, 6),
            "fnv1a64": "%016x" % fnv1a64(raw),
            "fnv1a64_pcm16": "%016x" % fnv1a64(pcm16(raw)),
        }
        checked += 1
        if any(got[k] != want[k] for k in got):
            bad += 1
            print("MANIFEST MISMATCH %s: %r != %r" % (name, got, want))
    print("manifest check: %d/%d entries match %s"
          % (checked - bad, checked, os.path.basename(path)))
    return 1 if bad else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def signed_range(slots) -> tuple[int, int]:
    """Signed min/max of an unsigned 32-bit accumulator array."""
    lo = hi = None
    for v in slots:
        sv = v - 0x100000000 if v & 0x80000000 else v
        if lo is None or sv < lo:
            lo = sv
        if hi is None or sv > hi:
            hi = sv
    return lo or 0, hi or 0


def write_wav(path: str, rate: int, channels: int, streams) -> None:
    """Write 16-bit PCM at the *source* rate (drop the duplicated slots)."""
    copies = factor_for(rate) // 2
    frames = len(streams[0]) // copies
    pcm = bytearray()
    for i in range(frames):
        for s in streams:
            v = (s[i * copies] & 0xFFFFFFFF)
            v = v - 0x100000000 if v & 0x80000000 else v
            v = max(-32768, min(32767, v >> PCM16_SHIFT))
            pcm += struct.pack("<h", v)
    with wave.open(path, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(bytes(pcm))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("name", nargs="?", help="entry name inside sounds/SOU.IDX")
    ap.add_argument("out", nargs="?", help="output .wav")
    ap.add_argument("--game", default=os.environ.get(
        "GUNMETAL_GAME", "/mnt/d/games/Gunmetal 3dfx"))
    ap.add_argument("--vol", type=int, default=31,
                    help="volume table index 0..31 (31 = full, default)")
    ap.add_argument("--list", action="store_true", help="list SOU1 entries")
    ap.add_argument("--verify", action="store_true",
                    help="compare the model with the real decoder bytes")
    ap.add_argument("--limit", type=int, help="verify only the first N entries")
    ap.add_argument("--json", help="write the per-entry FNV/duration manifest")
    ap.add_argument("--check", metavar="MANIFEST",
                    help="model-only check against a committed FNV manifest")
    ap.add_argument("--chunk", type=int, default=EMU_CHUNK,
                    help="emulator chunk in output slots (default 65536)")
    opts = ap.parse_args(argv)

    if opts.chunk <= 0 or opts.chunk % 8:
        ap.error("--chunk must be a positive multiple of 8")
    if opts.limit is not None and opts.limit < 0:
        ap.error("--limit must be non-negative")

    if opts.list:
        for name in list_entries(opts.game):
            print(name)
        return 0
    if opts.verify:
        return verify(opts.game, opts.limit, opts.json, opts.chunk)
    if opts.check:
        return check_manifest(opts.game, opts.check)
    if not opts.name or not opts.out:
        ap.error("NAME and OUT are required unless --list/--verify is used")
    data = read_entry(opts.name, opts.game)
    rate, channels, slots, streams = decode_entry(data, opts.vol)
    write_wav(opts.out, rate, channels, streams)
    print("%s: rate=%d channels=%d payload=%d slots/ch=%d (%.3f s) -> %s"
          % (opts.name, rate, channels, len(parse_entry(data)[2]), slots,
             slots / RATE_OUT, opts.out))
    for ch, stream in enumerate(streams):
        lo, hi = signed_range(stream)
        print("  channel %d raw accumulator range %d..%d" % (ch, lo, hi))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
