#pragma once

// ``.sml`` / ``.sme`` UI markup: document reader with parity to ``gm/sml.py``.
//
// The shipped ``.sml`` files are plain markup; ``.sme`` is the same text under
// the positional Caesar cipher (letter at byte ``i`` shifted by ``i % 10``
// inside its own alphabet).  A document is the flat tag stream ``<d> ... </d>``
// -- tags are *not* a tree -- so this API exposes the ordered elements plus a
// tag-stripped text, mirroring Python's ``Element``/``describe``.
//
// The cipher itself stays in ``res/text.h`` (``sml_decode``/``sml_encode``);
// this module adds the document layer (decoding variants, tag scanner,
// elements, summary) without changing that header.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gm {

/// Shift cycle: byte ``i`` of a letter moves by ``i % kSmlShiftMod`` letters.
inline constexpr int kSmlShiftMod = 10;

/// Loader limit: ``0x49779c`` rejects ``count >= 0x200``, so 511 tags fit.
inline constexpr std::size_t kSmlMaxTags = 512;
inline constexpr std::size_t kSmlTagLimit = kSmlMaxTags - 1;

/// How ``parse_sml`` treats its input.  ``Auto`` first tries the encoded form
/// (``decode_best``) and falls back to plain bytes, which is how the loader
/// prefers the ``.sme`` sibling.
enum class SmlSource { Auto, Plain, Encoded };

/// Element kinds, same spellings as Python's ``Element.kind``.
enum class SmlKind {
    Doc,
    Row,
    Paragraph,
    Column,
    Image,
    Font,
    Music,
    Placeholder,
    Text,
    Close,
    Unknown,
};

std::string_view sml_kind_name(SmlKind kind);

/// One ``name="value"`` attribute, raw spelling (case included).
struct SmlAttribute {
    std::string name;
    std::string value;
};

/// One UI element of a decoded document; ``pos`` is the byte offset in the
/// decoded markup and ``line`` is 1-based.  Text runs carry ``tag`` empty and
/// ``text``/``value`` set to the collapsed body.
struct SmlElement {
    SmlKind kind = SmlKind::Unknown;
    std::string tag;
    std::string value;
    std::vector<SmlAttribute> attrs;
    std::size_t line = 0;
    std::size_t pos = 0;
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> font;   // <f s=...>
    std::optional<int> track;  // <M B=...>
    std::optional<int> offset; // <M S=...>
    std::optional<char> align;
    std::string text;
};

/// One markup tag (``<...>``) of the flat stream.
struct SmlMarkupTag {
    std::string name;
    std::string raw_attrs; // text between the name and ``>`` (no closing '/')
    std::string raw;       // the full ``<...>`` text
    bool closing = false;
    bool self_closing = false;
    std::size_t pos = 0;
    std::vector<SmlAttribute> attrs;
};

/// A decoded and validated document.
struct SmlDocument {
    std::string text; // decoded markup (latin-1 byte per char)
    std::string note; // "as-is" / "newline-repair" / "crlf-strip" / "plain" / "unrepaired"
    SmlSource source = SmlSource::Auto;

    std::vector<SmlMarkupTag> tags;
    std::vector<SmlElement> elements;

    std::size_t chars = 0;
    std::size_t lines = 0;
    bool doc_open = false;
    bool doc_close = false;
    bool brackets_balanced = false;
    bool too_many_tags = false;
    std::vector<std::string> unknown_tags;

    std::size_t text_runs = 0;
    std::size_t placeholder_k = 0;
    std::size_t placeholder_s = 0;
    std::string plain;
    std::string body;
    std::string preview; // first 400 bytes of ``body``
};

/// Layout-free summary of a document (Python ``describe``).
struct SmlSummary {
    std::size_t chars = 0;
    std::size_t lines = 0;
    std::size_t tags = 0;
    std::size_t tag_limit = kSmlMaxTags;
    std::size_t images = 0;
    std::size_t fonts = 0;
    std::size_t music = 0;
    std::size_t text_runs = 0;
    std::size_t placeholder_k = 0;
    std::size_t placeholder_s = 0;
    bool too_many_tags = false;
    bool doc_open = false;
    bool doc_close = false;
    bool doc_ok = false;
    std::size_t bracket_open = 0;
    std::size_t bracket_close = 0;
    bool brackets_balanced = false;
    std::vector<std::string> tag_letters;
    std::vector<std::string> unknown_tags;
    std::vector<std::pair<std::string, std::size_t>> tag_counts;  // sorted by name
    std::vector<std::pair<std::string, std::size_t>> attributes; // sorted "tag.attr"
    std::string plain;
    std::string body;
    std::string preview;
};

// ---------------------------------------------------------------- decoding --
// The cipher (``sml_decode``/``sml_encode``) lives in ``res/text.h``; these
// wrap it with the transport repairs proven in docs/UI.md section 4.

/// Decode with latin-1 and, optionally, collapse a ``[\r\n]+`` run to one
/// space before decoding (fixes the stray CRLF in brief/level17.sme).
std::string sml_decode_text(std::span<const std::uint8_t> data, bool repair = false);

/// Every plausible decode, best effort: (note, text).  Notes are "as-is",
/// "newline-repair", "crlf-strip"; only valid ``<d>...</d>`` decodes.
std::vector<std::pair<std::string, std::string>>
sml_decode_candidates(std::span<const std::uint8_t> data);

/// Best decode plus the note that names the repair ("unrepaired" if none).
std::pair<std::string, std::string>
sml_decode_best(std::span<const std::uint8_t> data);

// ------------------------------------------------------------------ markup --

/// Tags of the markup in order (same grammar as Python's ``_TAG``: the name
/// starts with a letter, attributes need leading whitespace, quoted values).
std::vector<SmlMarkupTag> sml_scan_tags(std::string_view text);

/// Case-insensitive attribute lookup; nullptr when absent.
const std::string* sml_attribute(const SmlElement& element, std::string_view name);
const std::string* sml_attribute(const SmlMarkupTag& tag, std::string_view name);

/// Ordered UI elements (Python ``elements``); text runs included.
std::vector<SmlElement> sml_elements(std::string_view text);

// ---------------------------------------------------------------- document --

/// Decode and validate; on failure returns nullopt and sets ``error``.
///
/// Failures: empty input, no tags, first tag not `<d>`, last not `</d>`,
/// unbalanced ``<``/``>`` (which covers a truncated tail) or 512+ tags.
/// Unknown tag letters are reported in ``unknown_tags`` but are not fatal.
std::optional<SmlDocument> parse_sml(std::span<const std::uint8_t> data,
                                     std::string& error);
std::optional<SmlDocument> parse_sml(std::span<const std::uint8_t> data,
                                     SmlSource source, std::string& error);

/// Summary of a parsed document (Python ``describe``).
SmlSummary describe_sml(const SmlDocument& document);

} // namespace gm
