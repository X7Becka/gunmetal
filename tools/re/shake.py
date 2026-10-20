#!/usr/bin/env python3
"""Model of Gunmetal camera shake, view pitch and recoil (gundx.exe v1.36).

Stdlib only; the game installation is only read, never written.  This is a
*model* with a provenance table: every constant is referenced by virtual
address (image base 0x400000, listing ``/tmp/opencode/gundx.asm``).  The
derivation and the disproved hypotheses are in ``docs/SHAKE.md``.

Three independent mechanisms are modelled.

1. View pitch offset -- per-player ``p+0x2E60`` (absolute ``0x81bea0`` for
   player 0).  The first-person camera uses
   ``camera_pitch = player[0x18] + p[0x2E60]`` and the offset is clamped to
   +/-pi/2.  It is fed by a one-shot accumulator ``p+0x2F38`` that the analog
   look path fills; the "decay" branch exists but its timers are never
   written in this build.

2. Camera shake -- three *global* channels (pitch/roll/yaw) driven by the
   script commands ``SET_CAMERA_PITCH_SHAKE`` / ``..._ROLL_...`` /
   ``..._YAW_...``.  Each is a triangle wave added to the corresponding
   camera angle inside ``0x446590``:

       s = duration mod 4*period
       s = (s < 2*period) ? s - period : 3*period - s
       angle += amplitude * s / period

3. Recoil -- ``SOURCEPLAYER_RECOIL(D)``.  It is *not* a camera kick: it adds
   a horizontal velocity impulse to ``p+0x2D50/p+0x2D58`` opposite to the
   view's horizontal forward direction, scaled by ``|cos(view_pitch)|``.

Example::

    st = ShakeState()
    shake(st, {"pitch": 0.01}, dt=16.0)
    ch = ShakeChannel(); ch.set(0.05, 200, 800)
    cam = camera_angles(st, player_pitch=0.1, shake={"pitch": ch.value()})

    python3 tools/re/shake.py --selftest
    python3 tools/re/shake.py --json
    python3 tools/re/shake.py --trace --amp 0.05 --period 200 --duration 800
    python3 tools/re/shake.py --recoil 200 --yaw 30 --view-pitch 10
"""

import argparse
import json
import math
import sys
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------

IMAGE_BASE = 0x400000
PLAYER_BASE = 0x819040          # player 0; stride 0x3510 (0x44d918..0x44d938)
PLAYER_STRIDE = 0x3510
PLAYER_COUNT = 8                # handlers reject source >= 8 (0x459f4f/0x45a124)

# Player-relative offsets (absolute = PLAYER_BASE + offset).
PITCH = 0x18                    # 0x819058, torso/aim pitch, clamped +/-pi/6
ROLL = 0x20                     # 0x819060, clamped +/-20 deg
YAW = 0x28                      # 0x819068
EXT_VEL_X = 0x2D50              # 0x81bd90, recoil/knockback velocity
EXT_VEL_Y = 0x2D58              # 0x81bd98
PITCH_RATE = 0x2D60             # 0x81bda0
YAW_RATE_MAX = 0x2D98           # 0x81bdd8, read-only in this build
YAW_RATE = 0x2DA0               # 0x81bde0
ROLL_OFFSET = 0x2DE8            # 0x81be28
FALL_PITCH = 0x2DC0             # 0x81c000, SOURCEPLAYER_FALL_PITCH_DOWN
FALL_PITCH_RATE = 0x2DC8        # 0x81c008
VIEW_PITCH = 0x2E60             # 0x81bea0, "otdacha" in docs/CAMERA.md
VIEW_PITCH_MIN = 0x2E68         # 0x81bea8 = -pi/2
VIEW_PITCH_MAX = 0x2E70         # 0x81beb0 = +pi/2
PITCH_PENDING = 0x2F38          # 0x81bf78, one-shot pitch impulse
YAW_PENDING = 0x2F40            # 0x81bf80, pending yaw delta
DECAY_SCALE = 0x2F48            # 0x81bf88, 0.5 default
EYE_HEIGHT = 0x2F50             # 0x81bf90
DECAY_DIVISOR = 0x2FE0          # 0x81c020, zoom (1.0 in play)
FLAGS = 0x3200                  # 0x81c240
STATE = 0x3210                  # 0x81c250
ZERO_PITCH_FLAG = 0x3358        # 0x81c398, never written -> always 0
TIMER_A = 0x3360                # 0x81c3a0, never written -> always 0
TIMER_B = 0x3364                # 0x81c3a4, never written -> always 0
DECAY_TIMER_A = 0x3374          # 0x81c3b4, never written
DECAY_TIMER_B = 0x3378          # 0x81c3b8, never written

# Absolute globals.
G_SHAKE_PITCH_AMP = 0x80F828    # amplitude already divided by period
G_SHAKE_PITCH_PERIOD = 0x80F9A0
G_SHAKE_PITCH_DUR = 0x80D8BC
G_SHAKE_ROLL_AMP = 0x80F820
G_SHAKE_ROLL_PERIOD = 0x80D8B0
G_SHAKE_ROLL_DUR = 0x80D8C4
G_SHAKE_YAW_AMP = 0x80F838
G_SHAKE_YAW_PERIOD = 0x80F9C8
G_SHAKE_YAW_DUR = 0x80D7E8
G_ZOOM = 0x4AA390               # copied into p+0x2FE0 by 0x44d730

# Numeric constants (double, little-endian bytes in the exe).
C_DECAY_RATE = 0.016            # 0x4a2308, rad per timer unit
C_DECAY_THRESHOLD = 0.5         # 0x4a2310
C_DECAY_SLOPE = -1.5            # 0x4a2318
C_DECAY_INTERCEPT = 0.25        # 0x4a2320
C_DECAY_SCALE_DEFAULT = 0.5     # 0x4ac078 (queue default, 0x44d6b8)
C_DECAY_SCALE_DRIFT = -0.01     # 0x4a2388
C_HALF_PI = math.pi / 2.0       # 0x4a23a0
C_LOOK_SCALE = 1.0 / 1024.0     # 0x4a2430/0x4a2438 (+/-)
C_STATE7_PITCH = 0.01308996938995747   # 0x4a22f0, 0.75 deg
C_STATE7_ROLL = 0.019634954084936207   # 0x4a22f8, 1.125 deg
C_STATE7_DAMP = 0.95            # 0x4a22e8 is -0.95; code is 0.95*x + c
FRAME_DT_MAX_MS = 45            # 0x44d518 caps the frame delta at 0x2d
SENTINEL = 1e38                 # 0x4a2378, "no value"

XREF = {
    "view_pitch": (0x443A16, 0x443A36, 0x443FBB, 0x443FCF),
    "view_pitch_clamp": (0x443A72, 0x443A9D, 0x444015, 0x444040),
    "view_pitch_clear": (0x443A5E, 0x444001, 0x444C27),
    "pitch_pending": (0x443A03, 0x44452C),
    "yaw_pending": (0x4439AF, 0x44452C, 0x4445B1),
    "queue": (0x44d652, 0x44d6d6, 0x44d6f9, 0x44d750),
    "look_axis": (0x44c8cd, 0x44c91e, 0x44c92b, 0x44c989),
    "shake_set_pitch": (0x45b411,),
    "shake_set_roll": (0x45b462,),
    "shake_set_yaw": (0x45b4b3,),
    "shake_apply": (0x4467ef, 0x446831, 0x446873, 0x4468b5),
    "shake_tick": (0x457230, 0x457271),
    "recoil": (0x459f4d, 0x459f8e, 0x459fba),
    "camera_call": (0x446590, 0x41e025),
    "state7_fall": (0x443846, 0x44385B, 0x443874),
}


# --------------------------------------------------------------------------
# Validation helpers
# --------------------------------------------------------------------------

def _num(value, name):
    """Return *value* as a finite float or raise ValueError."""
    try:
        x = float(value)
    except (TypeError, ValueError):
        raise ValueError("%s: not a number: %r" % (name, value)) from None
    if not math.isfinite(x):
        raise ValueError("%s: NaN/Inf not allowed: %r" % (name, value))
    return x


def clamp(x, lo, hi):
    if lo > hi:
        raise ValueError("clamp: lo > hi (%r > %r)" % (lo, hi))
    return lo if x < lo else hi if x > hi else x


def decay_multiplier(scale):
    """0x443f6d..0x443f94: 1.0 below 0.5, else 0.25 + 1.5*scale."""
    scale = _num(scale, "decay_scale")
    if scale < C_DECAY_THRESHOLD:
        return 1.0
    return C_DECAY_INTERCEPT + (-C_DECAY_SLOPE) * scale


# --------------------------------------------------------------------------
# Camera shake channels
# --------------------------------------------------------------------------

@dataclass
class ShakeChannel:
    """One SET_CAMERA_*_SHAKE channel.

    ``period`` is the quarter period in milliseconds (the engine's modulo is
    ``4*period``), ``duration`` is the remaining time in milliseconds,
    ``amp_peak`` is the peak angle in radians.  The engine pre-divides the
    amplitude by the period (0x45b451); ``amp_scaled`` keeps that value.
    """

    amp_peak: float = 0.0
    period: int = 1
    duration: int = 0
    amp_scaled: float = field(default=0.0, repr=False)

    def set(self, amplitude, period, duration):
        """Mirror of the VM handler (0x45b411/0x45b462/0x45b4b3)."""
        amplitude = _num(amplitude, "amplitude")
        if isinstance(period, bool) or not isinstance(period, int):
            period = int(_num(period, "period"))
        if isinstance(duration, bool) or not isinstance(duration, int):
            duration = int(_num(duration, "duration"))
        if period <= 0:
            period = 1
        self.amp_peak = amplitude
        self.period = period
        self.duration = max(0, duration)
        self.amp_scaled = amplitude / period
        return self

    def tick(self, dt_ms):
        """0x457271..0x4572b3: duration -= dt, clamped at 0."""
        dt_ms = _num(dt_ms, "dt")
        if dt_ms < 0:
            raise ValueError("dt must be >= 0")
        self.duration = max(0, self.duration - int(dt_ms))
        return self

    def value(self):
        """Angle added to the camera this frame (0x4467ef..0x4468b1)."""
        if self.duration <= 0:
            return 0.0
        p = max(1, self.period)
        r = self.duration % (4 * p)
        s = r - p if r < 2 * p else 3 * p - r
        return self.amp_scaled * s


# --------------------------------------------------------------------------
# View pitch / yaw integrator
# --------------------------------------------------------------------------

@dataclass
class ShakeState:
    """Per-player camera state that the shake model touches."""

    view_pitch: float = 0.0       # p+0x2E60
    pitch_pending: float = 0.0    # p+0x2F38
    yaw: float = 0.0              # p+0x28
    yaw_pending: float = 0.0      # p+0x2F40
    yaw_rate: float = 0.0         # p+0x2DA0
    yaw_rate_max: float = 0.0     # p+0x2D98, rad per ms (0 = no limit here)
    decay_scale: float = C_DECAY_SCALE_DEFAULT   # p+0x2F48
    decay_divisor: float = 1.0    # p+0x2FE0, engine copies zoom here
    timer_a: int = 0              # p+0x3360
    timer_b: int = 0              # p+0x3364
    pitch_locked: bool = False    # p+0x3358 != 0 -> clear the offset


def _impulse_parts(impulse):
    if impulse is None:
        return 0.0, 0.0
    if isinstance(impulse, dict):
        return (_num(impulse.get("pitch", 0.0), "impulse.pitch"),
                _num(impulse.get("yaw", 0.0), "impulse.yaw"))
    return _num(impulse, "impulse"), 0.0


def shake(state, impulse=None, dt=0.0):
    """Advance one engine frame of the per-player view offset.

    ``state``   -- :class:`ShakeState` (mutated in place)
    ``impulse`` -- radians, or ``{"pitch": ..., "yaw": ...}``; equivalents of
                   the look-axis queue that fills p+0x2F38 / p+0x2F40
    ``dt``      -- frame time in **milliseconds** (0..45 in the engine)

    Returns a dict with the new ``view_pitch``, ``yaw``, ``pitch_step``,
    ``yaw_step`` and whether the decay branch ran.

    The pitch branch (0x443a03..0x443ac8) is exact::

        pending += impulse
        if pending != 0: view_pitch += pending; pending = 0
        else:            view_pitch -= dt * 0.016 * mult / divisor
        if flag: view_pitch = 0
        view_pitch = clamp(view_pitch, -pi/2, +pi/2)

    The yaw branch (0x44452c..0x444628) is simplified: the pending delta is
    applied and, when ``yaw_rate_max`` is positive, limited to
    ``yaw_rate_max * dt`` per frame.  See docs/SHAKE.md section 7.
    """
    dt = _num(dt, "dt")
    if dt < 0:
        raise ValueError("dt must be >= 0")

    d_pitch, d_yaw = _impulse_parts(impulse)
    state.pitch_pending = _num(state.pitch_pending, "pitch_pending") + d_pitch
    state.yaw_pending = _num(state.yaw_pending, "yaw_pending") + d_yaw

    decayed = False
    applied = 0.0
    if state.pitch_pending != 0.0:
        applied = state.pitch_pending
        state.view_pitch += applied
        state.pitch_pending = 0.0
    else:
        # Dormant in this build: TIMER_A/TIMER_B are never written, so the
        # difference is 0 and no decay happens.  Kept so the model matches
        # the code if a mode ever fills those fields.
        delta = state.timer_a - state.timer_b
        if delta:
            state.view_pitch -= (delta * C_DECAY_RATE
                                 * decay_multiplier(state.decay_scale)
                                 / _num(state.decay_divisor, "decay_divisor"))
            decayed = True

    if state.pitch_locked:
        state.view_pitch = 0.0
    state.view_pitch = clamp(state.view_pitch,
                             -C_HALF_PI, C_HALF_PI)

    yaw_step = 0.0
    if state.yaw_pending != 0.0:
        limit = _num(state.yaw_rate_max, "yaw_rate_max") * dt
        step = state.yaw_pending
        if limit > 0.0:
            step = clamp(step, -limit, limit)
        state.yaw += step
        state.yaw_pending -= step
        state.yaw_rate = 0.0
        yaw_step = step
    else:
        state.yaw += _num(state.yaw_rate, "yaw_rate") * dt

    return {
        "view_pitch": state.view_pitch,
        "yaw": state.yaw,
        "pitch_step": applied,
        "yaw_step": yaw_step,
        "decayed": decayed,
        "pending_pitch": state.pitch_pending,
        "pending_yaw": state.yaw_pending,
    }


def camera_angles(state, player_pitch=0.0, player_roll=0.0,
                  shake_pitch=0.0, shake_roll=0.0, shake_yaw=0.0):
    """Assemble the three angles passed to 0x41e025 (via 0x446590).

    ``b = player[0x18] + p[0x2E60] + shake_pitch``
    ``a = player[0x20] + shake_roll``
    ``c = player[0x28] + shake_yaw``
    """
    pitch = (_num(player_pitch, "player_pitch")
             + _num(state.view_pitch, "view_pitch")
             + _num(shake_pitch, "shake_pitch"))
    roll = _num(player_roll, "player_roll") + _num(shake_roll, "shake_roll")
    yaw = _num(state.yaw, "yaw") + _num(shake_yaw, "shake_yaw")
    return {"pitch": pitch, "roll": roll, "yaw": yaw}


# --------------------------------------------------------------------------
# Recoil
# --------------------------------------------------------------------------

def recoil_velocity(yaw, view_pitch, amount):
    """SOURCEPLAYER_RECOIL(D) at 0x459f4d..0x459fc7.

    ``vx -= sin(yaw) * |cos(view_pitch)| * D``
    ``vy -= cos(yaw) * |cos(view_pitch)| * D``

    Only the *view* pitch (p+0x2E60) is used, not the torso pitch; the
    vertical component is not touched (2-D pushback).
    """
    yaw = _num(yaw, "yaw")
    view_pitch = _num(view_pitch, "view_pitch")
    amount = _num(amount, "amount")
    k = math.cos(view_pitch)
    k = k if k >= 0.0 else -k
    return (-math.sin(yaw) * k * amount,
            -math.cos(yaw) * k * amount)


def fall_pitch_step(pitch, roll, dt_ms):
    """State-7 handler (0x443846..0x44388c), once per millisecond.

    pitch = 0.95*pitch + 0.75 deg, roll = 0.95*roll + 1.125 deg.
    Returns the pair after ``dt_ms`` iterations (the engine loops dt times).
    """
    pitch = _num(pitch, "pitch")
    roll = _num(roll, "roll")
    n = int(_num(dt_ms, "dt"))
    if n < 0:
        raise ValueError("dt must be >= 0")
    for _ in range(n):
        pitch = C_STATE7_DAMP * pitch + C_STATE7_PITCH
        roll = C_STATE7_DAMP * roll + C_STATE7_ROLL
    return pitch, roll


# --------------------------------------------------------------------------
# Calibration dump
# --------------------------------------------------------------------------

def calibration():
    """Machine-readable constants and provenance (``--json``)."""
    return {
        "exe": "gundx.exe v1.36",
        "image_base": IMAGE_BASE,
        "listing": "/tmp/opencode/gundx.asm (objdump -D -M intel)",
        "player": {
            "base": PLAYER_BASE,
            "stride": PLAYER_STRIDE,
            "max_players": PLAYER_COUNT,
        },
        "offsets": {
            "pitch": PITCH,
            "roll": ROLL,
            "yaw": YAW,
            "external_velocity_x": EXT_VEL_X,
            "external_velocity_y": EXT_VEL_Y,
            "pitch_rate": PITCH_RATE,
            "yaw_rate": YAW_RATE,
            "yaw_rate_max": YAW_RATE_MAX,
            "roll_offset": ROLL_OFFSET,
            "fall_pitch_target": FALL_PITCH,
            "fall_pitch_rate": FALL_PITCH_RATE,
            "view_pitch": VIEW_PITCH,
            "view_pitch_min": VIEW_PITCH_MIN,
            "view_pitch_max": VIEW_PITCH_MAX,
            "pitch_pending": PITCH_PENDING,
            "yaw_pending": YAW_PENDING,
            "decay_scale": DECAY_SCALE,
            "decay_divisor": DECAY_DIVISOR,
            "eye_height": EYE_HEIGHT,
            "flags": FLAGS,
            "state": STATE,
            "zero_pitch_flag": ZERO_PITCH_FLAG,
            "decay_timer_a": TIMER_A,
            "decay_timer_b": TIMER_B,
        },
        "absolute": {
            "shake_pitch_amp": G_SHAKE_PITCH_AMP,
            "shake_pitch_period": G_SHAKE_PITCH_PERIOD,
            "shake_pitch_duration": G_SHAKE_PITCH_DUR,
            "shake_roll_amp": G_SHAKE_ROLL_AMP,
            "shake_roll_period": G_SHAKE_ROLL_PERIOD,
            "shake_roll_duration": G_SHAKE_ROLL_DUR,
            "shake_yaw_amp": G_SHAKE_YAW_AMP,
            "shake_yaw_period": G_SHAKE_YAW_PERIOD,
            "shake_yaw_duration": G_SHAKE_YAW_DUR,
            "zoom": G_ZOOM,
        },
        "constants": {
            "decay_rate_rad_per_ms": C_DECAY_RATE,
            "decay_threshold": C_DECAY_THRESHOLD,
            "decay_slope": C_DECAY_SLOPE,
            "decay_intercept": C_DECAY_INTERCEPT,
            "decay_scale_default": C_DECAY_SCALE_DEFAULT,
            "look_rad_per_count": C_LOOK_SCALE,
            "view_pitch_clamp": [-C_HALF_PI, C_HALF_PI],
            "frame_dt_max_ms": FRAME_DT_MAX_MS,
            "no_value_sentinel": SENTINEL,
            "state7_damp": C_STATE7_DAMP,
            "state7_pitch_step": C_STATE7_PITCH,
            "state7_roll_step": C_STATE7_ROLL,
        },
        "xref": {k: ["0x%06x" % a for a in v] for k, v in XREF.items()},
        "commands": {
            "SET_CAMERA_PITCH_SHAKE": {"id": 0x4D, "handler": 0x45B411,
                                       "sig": "APP", "global": "0x80f828"},
            "SET_CAMERA_ROLL_SHAKE": {"id": 0x4E, "handler": 0x45B462,
                                      "sig": "APP", "global": "0x80f820"},
            "SET_CAMERA_YAW_SHAKE": {"id": 0x4F, "handler": 0x45B4B3,
                                     "sig": "APP", "global": "0x80f838"},
            "SOURCEPLAYER_RECOIL": {"id": 0x76, "handler": 0x459F4D,
                                    "sig": "D"},
            "SOURCEPLAYER_FALL_PITCH_DOWN": {"id": 0x91, "handler": 0x45A896,
                                             "sig": "DD"},
        },
        "formulas": {
            "camera_pitch": "player[0x18] + p[0x2E60] + shake.pitch",
            "camera_roll": "player[0x20] + shake.roll",
            "camera_yaw": "player[0x28] + shake.yaw",
            "shake_wave": ("s = duration mod 4p; "
                           "s = (s < 2p) ? s - p : 3p - s; "
                           "angle += amplitude * s / p"),
            "recoil": ("vx -= sin(yaw)*|cos(view_pitch)|*D; "
                       "vy -= cos(yaw)*|cos(view_pitch)|*D"),
        },
        "unproven": [
            "writers of p+0x3360/p+0x3364 (decay timers) — none in this build",
            "writer of yaw rate limit p+0x2D98 — none in this build",
            "native weapon-fire -> shake link — not found",
            "real script amplitudes/periods — no shipped .sbt uses the commands",
            "sign/branch of the yaw rate limiter 0x44457e",
        ],
    }


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def _close(a, b, eps=1e-12):
    return abs(a - b) <= eps * max(1.0, abs(a), abs(b))


def selftest(verbose=True):
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    # 1. +/-pi/2 clamp.
    st = ShakeState()
    shake(st, {"pitch": 10.0}, 16.0)
    check("clamp_high", _close(st.view_pitch, C_HALF_PI), st.view_pitch)
    st = ShakeState()
    shake(st, {"pitch": -10.0}, 16.0)
    check("clamp_low", _close(st.view_pitch, -C_HALF_PI), st.view_pitch)

    # 2. Impulse is consumed once, pending is cleared.
    st = ShakeState()
    shake(st, {"pitch": 0.25}, 16.0)
    check("impulse_applied", _close(st.view_pitch, 0.25))
    check("impulse_cleared", st.pitch_pending == 0.0)
    p = st.view_pitch
    shake(st, None, 16.0)
    check("no_decay_without_timers", st.view_pitch == p)

    # 3. Decay branch if the (dead) timers are non-zero.
    st = ShakeState(timer_a=10, timer_b=4, decay_scale=0.5, decay_divisor=1.0)
    shake(st, None, 16.0)
    check("decay_math", _close(st.view_pitch, -(6 * 0.016)), st.view_pitch)
    st = ShakeState(timer_a=10, timer_b=4, decay_scale=1.0, decay_divisor=2.0)
    shake(st, None, 16.0)
    check("decay_zoom", _close(st.view_pitch, -(6 * 0.016 * 1.75 / 2.0)),
          st.view_pitch)

    # 4. Decay multiplier breakpoints.
    check("mult_below", decay_multiplier(0.0) == 1.0)
    check("mult_049", decay_multiplier(0.49) == 1.0)
    check("mult_half", _close(decay_multiplier(0.5), 1.0))
    check("mult_one", _close(decay_multiplier(1.0), 1.75))

    # 5. Pitch lock clears the offset.
    st = ShakeState(view_pitch=0.3, pitch_locked=True)
    shake(st, None, 0.0)
    check("pitch_lock", st.view_pitch == 0.0)

    # 6. Triangle wave: peak at duration 4p and 2p, zero at p and 3p.
    ch = ShakeChannel().set(0.4, 100, 400)
    check("tri_start", _close(ch.value(), -0.4), ch.value())
    ch.duration = 100
    check("tri_zero1", _close(ch.value(), 0.0), ch.value())
    ch.duration = 200
    check("tri_peak", _close(ch.value(), 0.4), ch.value())
    ch.duration = 300
    check("tri_zero2", _close(ch.value(), 0.0), ch.value())
    ch.duration = 0
    check("tri_off", ch.value() == 0.0)

    # 7. Period 4p and range bounded by amp.
    ch = ShakeChannel().set(0.1, 37, 10000)
    vals = []
    ok = True
    for t in range(1, 1000):
        ch.duration = t
        v = ch.value()
        vals.append(v)
        ok = ok and -0.1 - 1e-15 <= v <= 0.1 + 1e-15
    check("tri_range", ok)
    ch.duration = 137
    v1 = ch.value()
    ch.duration = 137 + 4 * 37
    check("tri_period", _close(v1, ch.value()))

    # 8. Ticking a channel.
    ch = ShakeChannel().set(0.1, 10, 25)
    ch.tick(10)
    check("tick", ch.duration == 15)
    ch.tick(100)
    check("tick_floor", ch.duration == 0)

    # 9. Yaw: applied once, rate-limited when a max is set.
    st = ShakeState(yaw=1.0)
    shake(st, {"yaw": 0.5}, 10.0)
    check("yaw_apply", _close(st.yaw, 1.5) and st.yaw_pending == 0.0)
    st = ShakeState(yaw=0.0, yaw_rate_max=0.01)
    shake(st, {"yaw": 0.5}, 10.0)
    check("yaw_limit_step", _close(st.yaw, 0.1), st.yaw)
    check("yaw_limit_left", _close(st.yaw_pending, 0.4), st.yaw_pending)
    st = ShakeState(yaw=0.0, yaw_rate=0.02)
    shake(st, None, 10.0)
    check("yaw_rate", _close(st.yaw, 0.2), st.yaw)

    # 10. Camera assembly.
    st = ShakeState(view_pitch=0.1, yaw=0.2)
    cam = camera_angles(st, 0.3, 0.4, 0.01, 0.02, 0.03)
    check("cam_pitch", _close(cam["pitch"], 0.41), cam["pitch"])
    check("cam_roll", _close(cam["roll"], 0.42), cam["roll"])
    check("cam_yaw", _close(cam["yaw"], 0.23), cam["yaw"])

    # 11. Recoil geometry: backwards along the horizontal view vector.
    vx, vy = recoil_velocity(0.0, 0.0, 10.0)
    check("recoil_yaw0", _close(vx, 0.0) and _close(vy, -10.0), (vx, vy))
    vx, vy = recoil_velocity(math.pi / 2, 0.0, 10.0)
    check("recoil_yaw90", _close(vx, -10.0) and _close(vy, 0.0), (vx, vy))
    vx, vy = recoil_velocity(0.0, C_HALF_PI, 10.0)
    check("recoil_pitch90", _close(vx, 0.0) and _close(vy, 0.0), (vx, vy))
    vx, vy = recoil_velocity(0.0, math.pi / 3, 10.0)
    check("recoil_pitch60", _close(vy, -5.0), vy)

    # 12. State-7 fall converges to +15 deg pitch / +22.5 deg roll.
    p, r = fall_pitch_step(0.0, 0.0, 1000)
    check("fall_pitch", _close(p, C_STATE7_PITCH / (1 - C_STATE7_DAMP), 1e-9), p)
    check("fall_roll", _close(r, C_STATE7_ROLL / (1 - C_STATE7_DAMP), 1e-9), r)

    # 13. NaN / Inf / negative dt are rejected.
    for bad in (float("nan"), float("inf"), float("-inf")):
        try:
            shake(ShakeState(), {"pitch": bad}, 16.0)
        except ValueError:
            ok = True
        else:
            ok = False
        check("reject_%s" % bad, ok)
    try:
        shake(ShakeState(), None, -1.0)
    except ValueError:
        ok = True
    else:
        ok = False
    check("reject_neg_dt", ok)
    try:
        ShakeChannel().set(float("nan"), 1, 1)
    except ValueError:
        ok = True
    else:
        ok = False
    check("reject_shake_nan", ok)

    # 14. Serialisation of the calibration blob.
    cal = calibration()
    try:
        json.dumps(cal)
        ok = True
    except (TypeError, ValueError):
        ok = False
    check("json_serialisable", ok)

    failed = [c for c in checks if not c[1]]
    if verbose:
        print("shake.py selftest: %d/%d checks passed"
              % (len(checks) - len(failed), len(checks)))
        for name, _, detail in failed:
            print("  FAIL %s  %r" % (name, detail))
        if not failed:
            print("  clamp +/-pi/2, impulse/decay, triangle wave, recoil, "
                  "NaN guards")
    return 0 if not failed else 1


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _print_trace(args):
    ch = ShakeChannel().set(args.amp, args.period, args.duration)
    st = ShakeState()
    dt = min(float(args.dt), FRAME_DT_MAX_MS)
    rows = []
    t = 0.0
    for i in range(args.frames):
        v = ch.value()
        cam = camera_angles(st, args.player_pitch, 0.0, v, 0.0, 0.0)
        rows.append({
            "frame": i,
            "t_ms": round(t, 3),
            "duration_ms": ch.duration,
            "shake_pitch": v,
            "camera_pitch_deg": math.degrees(cam["pitch"]),
        })
        ch.tick(dt)
        t += dt
    if args.json:
        print(json.dumps(rows, indent=2))
    else:
        print("frame  t_ms  dur_ms  shake_rad  cam_pitch_deg")
        for r in rows:
            print("%5d %6.1f %7d %10.5f %14.4f"
                  % (r["frame"], r["t_ms"], r["duration_ms"],
                     r["shake_pitch"], r["camera_pitch_deg"]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Gunmetal shake/view-pitch/recoil model (stdlib only)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                    help="run the numeric self-test")
    ap.add_argument("--json", action="store_true",
                    help="print the calibration as JSON")
    ap.add_argument("--trace", action="store_true",
                    help="print a SET_CAMERA_PITCH_SHAKE trace")
    ap.add_argument("--amp", type=float, default=0.05,
                    help="shake amplitude, radians (trace)")
    ap.add_argument("--period", type=int, default=200,
                    help="shake quarter period, ms (trace)")
    ap.add_argument("--duration", type=int, default=800,
                    help="shake duration, ms (trace)")
    ap.add_argument("--dt", type=float, default=16.0,
                    help="frame delta, ms (trace)")
    ap.add_argument("--frames", type=int, default=12,
                    help="frames to print (trace)")
    ap.add_argument("--player-pitch", type=float, default=0.0,
                    help="player[0x18], radians (trace)")
    ap.add_argument("--recoil", type=float, metavar="D",
                    help="print SOURCEPLAYER_RECOIL(D) velocity")
    ap.add_argument("--yaw", type=float, default=0.0,
                    help="yaw, degrees (recoil)")
    ap.add_argument("--view-pitch", type=float, default=0.0,
                    help="view pitch p+0x2E60, degrees (recoil)")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.recoil is not None:
        vx, vy = recoil_velocity(math.radians(args.yaw),
                                 math.radians(args.view_pitch),
                                 args.recoil)
        out = {"vx": vx, "vy": vy, "speed": math.hypot(vx, vy)}
        print(json.dumps(out, indent=2) if args.json else
              "recoil: vx=%.6f vy=%.6f |v|=%.6f" % (vx, vy, math.hypot(vx, vy)))
        return 0
    if args.trace:
        return _print_trace(args)
    if args.json:
        print(json.dumps(calibration(), indent=2, sort_keys=True))
        return 0

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
