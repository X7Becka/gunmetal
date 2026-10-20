#pragma once

// HUD binding: link ``.sml``/``.sme`` screens to the ``.sof``/``.fdx`` film
// families and to the element positions carried by the markup.
//
// The engine has two independent UI layers (docs/UI.md §7.4):
//
//   * SML screens - static briefings/design pages drawn by the pager
//     ``0x497880``; images are loose 64x64 BMPs referenced by ``<I f=...>``;
//   * HUD films - ``Film/<FAMILY><id>.sof`` animated overlays drawn by
//     ``film_set`` (``0x4958d0``) on line 125 of the screen, with
//     ``<family>.fdx`` supplying ``count,fps``.
//
// This module is the *typing glue* between the two: it classifies a screen by
// its name (``DESIGN/WEAPON15`` -> family WEAPON, id 15), derives the film
// name/line, and turns the ``<I x y a>`` elements into concrete placement
// rectangles.  It never loads a screen on its own and never writes the game
// directory; ``resolve_film`` is the only function that touches the filesystem
// and it only reads ``Film/*.sof`` + ``Film/*.fdx``.
//
// Proven vs hypothesis
// --------------------
// Each record carries a ``HudBasis``.  ``Proven`` is used only where an xref
// pins the behaviour; everything that is inferred from data conventions or
// from a partially decoded call site is ``Hypothesis`` and is spelled out in
// ``note``.  In particular:
//
//   * Proven (``0x4958d0``, docs/FILM.md §1.3/§2/§3): ``film_set(prefix, id,
//     ...)`` formats ``"%s%ld"`` for the file name and passes the *same* id to
//     the ``.fdx`` line parser, so ``WEAPON8.SOF`` is ``weapon.fdx`` line 8;
//     the HUD overlay sits at ``x + 125*pitch + 14``, i.e. (14, 125) for a
//     320x200 surface; the family prefixes are literals in the call sites
//     (``WEAPON`` 0x4be894, ``AMMO`` 0x4be724, ``TANK`` 0x4be7e8, ``SHIELD``
//     0x4be7e0, ``EQUIP`` 0x4be778, ``DEFAULT`` 0x4bb344).
//   * Proven (``0x498012``, docs/UI.md §5.2): ``<I x y f a>`` pixels, ``a=l``
//     pins the left edge, ``a=r`` the right edge; the shipped 72 images are
//     all ``x=64 y=64`` and 64x64 BMPs.
//   * Hypothesis: that a *design screen* named ``WEAPON15`` is the page that
//     calls ``film_set("WEAPON", 15)``.  The names line up 1:1 with the film
//     families and the garage call path (``0x496d82``, which loads
//     ``SML/DESIGN/TEMP0.SML``) proves the *mechanism*, but no per-screen xref
//     was recovered.  ``resolve_film`` therefore marks every state-derived
//     choice as ``Hypothesis``.
//   * Hypothesis: that ``<I>`` slots are the film anchors.  In the shipped
//     data every ``<I>`` references a loose BMP, never a film; using the slot
//     as a film position is this module's contract, not an engine behaviour.
//   * Hypothesis: that the ``a`` edge is measured against the screen edges
//     (0 / width-128).  The handler measures against the current text column
//     (0x68e654/0x68e64c), whose origin for the pager is not pinned
//     (docs/UI.md §10.5).  With ``a`` present the ``x`` attribute is recorded
//     as ``x_reserved`` but does not move the placement.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_player.h"
#include "render/render.h"
#include "res/sml.h"

namespace gm {

/// Film families of ``0x4958d0``.  ``Default`` is the fallback film
/// (``Default.sof`` / line 0) that the loader uses when nothing else applies.
enum class HudFamily { Default, Weapon, Ammo, Tank, Shield, Equip };

/// How sure the record is: xref-proven or inferred (see the file comment).
enum class HudBasis { Proven, Hypothesis };

/// ``<I a=...>`` alignment: no attribute, left edge or right edge.
enum class HudAnchor { None, Left, Right };

std::string_view hud_family_name(HudFamily family);
std::string_view hud_family_prefix(HudFamily family); ///< "WEAPON", ..., "DEFAULT"
std::string_view hud_basis_name(HudBasis basis);
std::string_view hud_anchor_name(HudAnchor anchor);

/// Per-screen limits.  A document with more image slots is rejected instead of
/// being silently truncated (the shipped maximum on one screen is 3).
inline constexpr std::size_t kHudBindMaxImages = 32;

/// Coordinates outside +-4096 are rejected: no shipped screen comes close,
/// and the bound keeps the clip arithmetic far from overflow.
inline constexpr int kHudBindMaxCoord = 4096;

/// Reference surface of the SML pager and the HUD overlay (docs/UI.md §5.5).
inline constexpr int kHudBindScreenWidth = 320;
inline constexpr int kHudBindScreenHeight = 200;

/// HUD film origin on a 320x200 surface: ``dest = x + 125*pitch + 14``
/// (``0x495a08``, docs/FILM.md §1.3).
inline constexpr int kHudBindFilmX = 14;
inline constexpr int kHudBindFilmY = 125;

/// One screen name split into the film coordinates it implies.
///
/// ``stem`` is the basename without extension, upper-cased.  ``has_id`` is set
/// only for ``<FAMILY><digits>`` (and ``DEFAULT``); ``basis`` is ``Proven`` for
/// those names, because the family prefixes are xref'd literals and the id is
/// the ``.fdx`` line by construction.  An unrecognised name yields
/// ``Default``/``Hypothesis`` - the caller then needs the player state.
struct HudScreenClass {
    std::string stem;
    HudFamily family = HudFamily::Default;
    bool has_id = false;
    int id = 0;
    HudBasis basis = HudBasis::Hypothesis;
};

/// Classify ``screen`` (a path, basename or bare stem).  Never fails: unknown
/// names are reported as ``Default`` so a caller can still ask the state.
HudScreenClass classify_screen(std::string_view screen);

/// One ``<I>`` slot of a screen, already validated.
struct HudElementBinding {
    std::size_t index = 0;  ///< 0-based ordinal among the screen's images
    std::size_t line = 0;   ///< 1-based line of the ``<I>`` in the markup
    std::string image;      ///< ``f`` value verbatim (loose BMP name)
    int x = 0;              ///< ``x`` attribute (0 when only the anchor is used)
    int y = 0;              ///< ``y`` attribute
    HudAnchor anchor = HudAnchor::None;
    HudBasis position_basis = HudBasis::Proven;
    bool x_reserved = false; ///< anchor is set, so ``x`` does not move the slot
};

/// A screen bound to its film coordinates plus its image slots.
struct HudBinding {
    std::string screen; ///< verbatim argument of ``parse_binding``
    HudScreenClass screen_class;
    std::vector<HudElementBinding> images;

    HudFamily family() const { return screen_class.family; }
    bool has_id() const { return screen_class.has_id; }
    int id() const { return screen_class.id; }
};

/// Parse the binding of *doc*.  ``screen`` names the source (path/stem) and
/// drives ``classify_screen``; it may be empty, in which case the binding is
/// ``Default`` and only the ``<I>`` slots are meaningful.
///
/// Rejects, with *error* set:
///   * an empty document, no tags, missing ``<d>``/``</d>``, unbalanced
///     brackets or the 512-tag limit (the same shape checks as ``parse_sml``,
///     repeated so a hand-built ``SmlDocument`` cannot slip through);
///   * an ``<I>`` without ``f``, without ``y``, without ``x`` when there is no
///     anchor, with a non-integer ``x``/``y``, or with ``a`` other than
///     ``l``/``r``;
///   * coordinates outside +-kHudBindMaxCoord;
///   * more than kHudBindMaxImages slots.
/// Nothing is skipped silently: the message names the 1-based line.
std::optional<HudBinding> parse_binding(const SmlDocument& doc,
                                        std::string_view screen,
                                        std::string& error);

/// Player state used when the screen name does not carry an id.  ``-1`` means
/// "not known"; ``shield`` has id 0 in the shipment but is still explicit.
/// ``focus`` says which family a generic screen (``PRELEVEL/LEVEL3``, a status
/// panel) should show; it is the caller's intent, not an engine global.
///
/// The engine reads ``0x81c4a0`` for both WEAPON and AMMO (docs/FILM.md §3);
/// whether that field is a weapon slot or an ammo type is still a hypothesis
/// (docs/FILM.md §9.1), which is why state-derived bindings are never
/// ``Proven``.
struct HudState {
    HudFamily focus = HudFamily::Weapon;
    int weapon = -1;
    int ammo = -1;
    int tank = -1;
    int equip = -1;
    int shield = -1;

    /// Slot for *family*, ``-1`` when unknown.  ``Default`` always yields 0.
    int slot(HudFamily family) const;
};

/// A film name/line resolved for a binding, with the loaded assets.
struct HudFilmRef {
    HudFamily family = HudFamily::Default;
    HudBasis basis = HudBasis::Hypothesis;
    int id = 0;
    std::string name; ///< "WEAPON8", "Default"
    std::string note; ///< why this family/id was chosen
    HudFilm film;
};

/// Resolve and load the film for *binding*.
///
/// Order (first match wins):
///   1. the screen name has an id -> that family/id (``Proven``);
///   2. the screen names a family and *state* has its slot -> that pair
///      (``Hypothesis``);
///   3. ``state.focus`` has a slot -> that family/slot (``Hypothesis``);
///   4. ``Default.sof`` (``Hypothesis``, ``note`` explains the fallback).
///
/// Errors come from ``load_hud_film`` (missing ``.sof``/``.fdx``, line outside
/// the index, bad ``count``/``fps``) and are prefixed with the resolved name so
/// a caller sees *which* film failed.  ``EQUIP<n>`` is the expected failure:
/// ``equip.fdx`` exists but no ``EQUIP*.SOF`` was shipped (docs/FILM.md §2).
std::optional<HudFilmRef> resolve_film(const HudBinding& binding, const HudState& state,
                                       const std::filesystem::path& game,
                                       std::string& error);

/// One concrete film rectangle on the target surface.
struct HudPlacement {
    std::size_t index = 0; ///< slot ordinal (0 for the implicit HUD overlay)
    std::string image;     ///< ``<I f=...>`` or empty for the overlay placement
    HudAnchor anchor = HudAnchor::None;
    HudBasis basis = HudBasis::Proven;
    int x = 0;
    int y = 0;
    int width = kSofTileWidth;
    int height = kSofTileHeight;
    bool clipped = false;    ///< part of the rectangle is outside the target
    bool visible = false;    ///< the intersection is non-empty
    int clip_width = 0;      ///< width of the visible part
    int clip_height = 0;     ///< height of the visible part
};

/// Layout the film's 128x64 rectangle for every image slot of *binding*.
///
///   * no ``<I>`` slots -> one placement at (14, 125), ``Proven``: the screen
///     is a full-page design/briefing and the film is the overlay of
///     ``0x4958d0``;
///   * ``a=l`` -> ``x = 0``, ``a=r`` -> ``x = target.width - 128`` (the anchor
///     hypothesis, see the file comment), no ``a`` -> the ``x`` attribute;
///   * ``y`` always comes from the attribute.
///
/// Off-screen rectangles are kept and marked (``clipped``/``visible``); a
/// fully invisible slot is not dropped, so a caller can report it.  Fails with
/// *error* on an invalid target (dimensions outside 1..8192 or an ``rgb``
/// buffer that is not ``width*height*3`` bytes) and on an unusable film
/// (``count``/``fps``/tiles, via the same checks as ``frame_at``).
std::optional<std::vector<HudPlacement>> layout(const HudBinding& binding,
                                               const HudFilm& film,
                                               const RenderResult& target,
                                               std::string& error);

/// ``layout(parse_binding(doc, "", error), film, target, error)``; the screen
/// name is irrelevant to the geometry.
std::optional<std::vector<HudPlacement>> layout(const SmlDocument& doc,
                                               const HudFilm& film,
                                               const RenderResult& target,
                                               std::string& error);

} // namespace gm
