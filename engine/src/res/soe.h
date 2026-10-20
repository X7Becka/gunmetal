#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

// A ``.soe`` file is the text source of one 936-byte ``THING_TYPES`` record of
// a saved ``.sbt`` (docs/SOE.md, docs/FORMATS.md §13).  The shipped corpus is
// one file (``SOBS/railrock.soe``, 340 bytes, 67 tokens); the grammar below was
// recovered from the loader ``gundx.exe`` VMA 0x47cb6c, not from samples.

/// Size of one ``THING_TYPES`` record: ``char name[16]`` + 49 ``double`` +
/// 528 bytes of integers (loader stride at 0x776d60).
constexpr std::size_t kSoeRecordSize = 0x3A8;

/// Safety limits (docs/SECURITY.md).  The first three are far above the
/// shipped data; the four counts are the loader's own clamps (0x47dc2c
/// weapons, 0x47ddad children, 0x47daea path points).
constexpr std::size_t kSoeMaxBytes = 1u << 20;
constexpr std::size_t kSoeMaxTokens = 1u << 16;
constexpr std::size_t kSoeMaxTokenBytes = 4096;
constexpr int kSoeMaxVersion = 13;
constexpr int kSoeMaxWeapons = 8;
constexpr int kSoeMaxChildren = 4;
constexpr int kSoeMaxPathPoints = 32;

/// One value read from the token stream.  Python ``gm.soe.Field.kind`` uses
/// ``"int"``/``"flt"``/``"str"``; the C++ enum keeps the same three cases.
enum class SoeValueKind { Int, Double, String, Name16 };
enum class SoeStatus { Proven, Hypothesis, Unknown };

const char* soe_kind_name(SoeValueKind kind);
const char* soe_status_name(SoeStatus status);

/// One parser token plus its engine destination (when the field map knows it).
struct SoeField {
    std::string label;        ///< parser label (``f1d4``, ``weapon[0].w2``, ...)
    SoeValueKind kind = SoeValueKind::Int;
    std::int64_t i = 0;       ///< valid for ``Int``
    double d = 0.0;           ///< valid for ``Double``
    std::string s;            ///< valid for ``String``
    int offset = -1;          ///< byte offset in THING_TYPES, -1 when unknown
};

/// One ``offset -> role`` row of the 77-entry record map (``gm/soe.py``
/// ``FIELD_MAP``).  ``size`` is the width of one element, ``count``/``stride``
/// describe arrays, ``soe``/``legacy`` are parser labels, ``xref`` is the
/// comma-separated consumer evidence from the reverse-engineering notes.
struct SoeFieldInfo {
    int offset = 0;
    int size = 0;
    SoeValueKind kind = SoeValueKind::Int;
    std::string_view name;
    std::string_view role;
    SoeStatus status = SoeStatus::Unknown;
    std::string_view soe;     ///< parser label or ``prefix[i]`` pattern
    std::string_view legacy;  ///< old label kept for gm.py compatibility
    int count = 1;
    int stride = 0;
    int instance = -1;        ///< runtime instance offset, -1 when unmapped
    std::string_view loader;  ///< loader format string / address
    std::string_view xref;    ///< comma-separated consumer xrefs
};

/// The 77 rows of ``FIELD_MAP`` in offset order.
std::span<const SoeFieldInfo> field_map();

/// Map a parser label (``f1d4``, ``weapon[0].w2``, ``child[1].c3``, ...) to its
/// record-map row; ``nullptr`` for ``version``-like labels with no destination.
const SoeFieldInfo* label_info(std::string_view label);

/// One compiled thing type: ``name`` is the thing-type name (the ``.soe`` file
/// stem, i.e. the ``THING_TYPES`` search key), ``id`` is the ``class_name``
/// token resolved through ``SOB_TABLE`` (record ``+0x1a4``).
struct ThingType {
    std::string name;
    std::string id;
    int version = 0;
    std::vector<SoeField> fields;
    std::size_t extra = 0; ///< trailing tokens; 0 for a well-formed file

    /// Field by parser label (array labels keep their ``[i]`` suffix).
    const SoeField* find(std::string_view label) const;
    std::int64_t get_int(std::string_view label, std::int64_t fallback = 0) const;
    double get_double(std::string_view label, double fallback = 0.0) const;
    std::string_view get_string(std::string_view label,
                                std::string_view fallback = {}) const;

    /// ``true`` when the whole token stream was consumed.
    bool complete() const { return extra == 0; }
};

/// Parsed ``.soe`` file.  The grammar has no record separator (there is no
/// ``DEFINE_THING_TYPES`` directive; ``THING_TYPES`` is the binary record in
/// ``.sbt``), so ``records`` always holds exactly one element on success.  The
/// vector keeps the Python ``ThingType`` vocabulary and leaves room for a
/// future multi-record wrapper.
struct SoeFile {
    std::string name;              ///< file stem / thing-type name (optional)
    std::string source;            ///< exact input bytes, for to_bytes()
    std::vector<ThingType> records;

    const ThingType& thing() const { return records.front(); }
    std::vector<std::uint8_t> to_bytes() const;
};

/// Parse a ``.soe`` token stream.  ``name`` (optional) is the thing-type name,
/// normally the file stem: it is copied into the record's ``name[16]`` by the
/// loader, not read from the file.  On failure returns ``nullopt`` and sets
/// ``error``; the accepted grammar and the strictness differences against
/// ``gm/soe.py`` are documented in docs/SOE_CPP.md.
std::optional<SoeFile> parse_soe(std::span<const std::uint8_t> data,
                                 std::string& error,
                                 std::string_view name = {});

} // namespace gm
