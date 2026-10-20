#include "res/sml.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <map>
#include <set>

#include "res/text.h"

namespace gm {
namespace {

bool is_space(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isspace(u) != 0;
}

bool is_alpha(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string lower_copy(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lower_char(c);
    }
    return out;
}

std::string_view trim_view(std::string_view text) {
    std::size_t b = 0;
    std::size_t e = text.size();
    while (b < e && is_space(text[b])) {
        ++b;
    }
    while (e > b && is_space(text[e - 1])) {
        --e;
    }
    return text.substr(b, e - b);
}

std::string latin1(std::span<const std::uint8_t> data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

/// Python ``int(value.strip())``: optional sign, decimal digits, full string.
std::optional<int> parse_int(std::string_view text) {
    const std::string_view t = trim_view(text);
    if (t.empty()) {
        return std::nullopt;
    }
    std::size_t i = 0;
    if (t[0] == '+' || t[0] == '-') {
        ++i;
    }
    if (i == t.size()) {
        return std::nullopt;
    }
    for (std::size_t k = i; k < t.size(); ++k) {
        if (t[k] < '0' || t[k] > '9') {
            return std::nullopt;
        }
    }
    const std::string body(t);
    const long long value = std::strtoll(body.c_str(), nullptr, 10);
    if (value < INT_MIN || value > INT_MAX) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

// Attribute grammar: ``[A-Za-z][A-Za-z0-9]*\s*=\s*"([^"]*)"``.
void parse_attributes(std::string_view raw, std::vector<SmlAttribute>& out) {
    std::size_t i = 0;
    while (i < raw.size()) {
        if (!is_alpha(raw[i])) {
            ++i;
            continue;
        }
        std::size_t p = i + 1;
        while (p < raw.size() && is_alnum(raw[p])) {
            ++p;
        }
        const std::string name(raw.substr(i, p - i));
        while (p < raw.size() && is_space(raw[p])) {
            ++p;
        }
        if (p >= raw.size() || raw[p] != '=') {
            ++i;
            continue;
        }
        ++p;
        while (p < raw.size() && is_space(raw[p])) {
            ++p;
        }
        if (p >= raw.size() || raw[p] != '"') {
            ++i;
            continue;
        }
        const std::size_t value_start = ++p;
        while (p < raw.size() && raw[p] != '"') {
            ++p;
        }
        if (p >= raw.size()) {
            ++i;
            continue;
        }
        const std::string value(raw.substr(value_start, p - value_start));
        // Python dict: first position wins, a repeated key keeps the last value.
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const SmlAttribute& a) { return a.name == name; });
        if (it != out.end()) {
            it->value = value;
        } else {
            out.push_back(SmlAttribute{name, value});
        }
        i = p + 1;
    }
}

const std::string* find_attr(const std::vector<SmlAttribute>& attrs, std::string_view name) {
    for (const auto& attr : attrs) {
        if (lower_copy(attr.name) == lower_copy(name)) {
            return &attr.value;
        }
    }
    return nullptr;
}

std::optional<int> int_attr(const std::vector<SmlAttribute>& attrs,
                            std::initializer_list<std::string_view> names) {
    for (std::string_view name : names) {
        if (const std::string* value = find_attr(attrs, name)) {
            return parse_int(*value);
        }
    }
    return std::nullopt;
}

std::string decimal(std::optional<int> value) {
    return value ? std::to_string(*value) : std::string();
}

const std::set<std::string>& shipped_tags() {
    static const std::set<std::string> tags = {
        "d", "D", "r", "R", "f", "F", "p", "P", "c", "C", "s", "S", "k", "I", "M",
    };
    return tags;
}

const std::map<std::string, std::string>& placeholders() {
    static const std::map<std::string, std::string> values = {
        {"k", "Soldier"}, {"K", "Soldier"}, {"s", " "}, {"S", " "},
    };
    return values;
}

/// Python ``_valid_document``: wrapper plus only shipped tag letters.
bool valid_document(const std::vector<SmlMarkupTag>& tags) {
    if (tags.empty()) {
        return false;
    }
    if (tags.front().closing || lower_copy(tags.front().name) != "d") {
        return false;
    }
    if (!tags.back().closing || lower_copy(tags.back().name) != "d") {
        return false;
    }
    for (const auto& tag : tags) {
        if (!tag.closing && shipped_tags().count(tag.name) == 0) {
            return false;
        }
    }
    return true;
}

std::string collapse_ws(std::string_view text) {
    std::string out;
    bool pending = false;
    for (char c : text) {
        if (is_space(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
    }
    return out;
}

/// Python ``plain``: block tags become newlines, placeholders are substituted,
/// attribute-carrying tags stay verbatim, every line is trimmed/skipped.
std::string plain_of(std::string_view text) {
    std::string raw;
    std::size_t last = 0;
    for (const auto& tag : sml_scan_tags(text)) {
        raw.append(text.substr(last, tag.pos - last));
        last = tag.pos + tag.raw.size();
        if (tag.closing) {
            continue;
        }
        const std::string low = lower_copy(tag.name);
        if (low == "r" || low == "p") {
            raw.push_back('\n');
        } else if (low == "c") {
            raw.append("  ");
        } else {
            auto it = placeholders().find(tag.name);
            if (it != placeholders().end()) {
                raw.append(it->second);
            } else if (!trim_view(tag.raw_attrs).empty()) {
                raw.append(tag.raw);
            }
        }
    }
    raw.append(text.substr(last));

    std::string result;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find('\n', start);
        if (end == std::string::npos) {
            end = raw.size();
        }
        const std::string_view line = trim_view(std::string_view(raw).substr(start, end - start));
        if (!line.empty()) {
            if (!result.empty()) {
                result.push_back('\n');
            }
            result.append(line);
        }
        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string strip_tags(std::string_view text) {
    std::string out;
    std::size_t last = 0;
    for (const auto& tag : sml_scan_tags(text)) {
        out.append(text.substr(last, tag.pos - last));
        last = tag.pos + tag.raw.size();
    }
    out.append(text.substr(last));
    return collapse_ws(out);
}

const char* structural_error(std::string_view text, const std::vector<SmlMarkupTag>& tags) {
    if (tags.empty()) {
        return "sml: no tags";
    }
    if (tags.front().closing || lower_copy(tags.front().name) != "d") {
        return "sml: first tag is not <d>";
    }
    if (!tags.back().closing || lower_copy(tags.back().name) != "d") {
        return "sml: last tag is not </d>";
    }
    const std::size_t open =
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '<'));
    const std::size_t close =
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '>'));
    if (open != close) {
        return "sml: bracket imbalance";
    }
    if (tags.size() >= kSmlMaxTags) {
        return "sml: tag limit exceeded";
    }
    return nullptr;
}

void fill_document(SmlDocument& doc, std::string text, std::string note, SmlSource source) {
    doc.text = std::move(text);
    doc.note = std::move(note);
    doc.source = source;
    doc.tags = sml_scan_tags(doc.text);
    doc.elements = sml_elements(doc.text);
    doc.chars = doc.text.size();
    doc.lines =
        static_cast<std::size_t>(std::count(doc.text.begin(), doc.text.end(), '\n')) + 1;
    doc.doc_open = !doc.tags.empty() && !doc.tags.front().closing &&
                   lower_copy(doc.tags.front().name) == "d";
    doc.doc_close = !doc.tags.empty() && doc.tags.back().closing &&
                    lower_copy(doc.tags.back().name) == "d";
    doc.brackets_balanced = std::count(doc.text.begin(), doc.text.end(), '<') ==
                            std::count(doc.text.begin(), doc.text.end(), '>');
    doc.too_many_tags = doc.tags.size() >= kSmlMaxTags;

    std::set<std::string> known;
    for (const auto& tag : doc.tags) {
        if (!tag.closing) {
            known.insert(tag.name);
        }
    }
    for (const auto& name : known) {
        if (shipped_tags().count(name) == 0) {
            doc.unknown_tags.push_back(name);
        }
    }
    std::sort(doc.unknown_tags.begin(), doc.unknown_tags.end());

    for (const auto& element : doc.elements) {
        if (element.kind == SmlKind::Text) {
            ++doc.text_runs;
        } else if (element.kind == SmlKind::Placeholder) {
            const std::string low = lower_copy(element.tag);
            if (low == "k") {
                ++doc.placeholder_k;
            } else if (low == "s") {
                ++doc.placeholder_s;
            }
        }
    }
    doc.plain = plain_of(doc.text);
    doc.body = strip_tags(doc.plain);
    doc.preview = doc.body.substr(0, 400);
}

} // namespace

std::string_view sml_kind_name(SmlKind kind) {
    switch (kind) {
    case SmlKind::Doc: return "doc";
    case SmlKind::Row: return "row";
    case SmlKind::Paragraph: return "paragraph";
    case SmlKind::Column: return "column";
    case SmlKind::Image: return "image";
    case SmlKind::Font: return "font";
    case SmlKind::Music: return "music";
    case SmlKind::Placeholder: return "placeholder";
    case SmlKind::Text: return "text";
    case SmlKind::Close: return "close";
    case SmlKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string sml_decode_text(std::span<const std::uint8_t> data, bool repair) {
    std::vector<std::uint8_t> input(data.begin(), data.end());
    if (repair && (std::find(input.begin(), input.end(), '\r') != input.end() ||
                   std::find(input.begin(), input.end(), '\n') != input.end())) {
        std::vector<std::uint8_t> fixed;
        fixed.reserve(input.size());
        std::size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '\r' || input[i] == '\n') {
                while (i < input.size() && (input[i] == '\r' || input[i] == '\n')) {
                    ++i;
                }
                fixed.push_back(' ');
            } else {
                fixed.push_back(input[i]);
                ++i;
            }
        }
        input.swap(fixed);
    }
    return latin1(sml_decode(input));
}

std::vector<std::pair<std::string, std::string>>
sml_decode_candidates(std::span<const std::uint8_t> data) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string raw = sml_decode_text(data);
    if (valid_document(sml_scan_tags(raw))) {
        out.emplace_back("as-is", raw);
    }
    const std::string repaired = sml_decode_text(data, true);
    if (valid_document(sml_scan_tags(repaired)) &&
        std::none_of(out.begin(), out.end(),
                     [&](const auto& item) { return item.second == repaired; })) {
        out.emplace_back("newline-repair", repaired);
    }
    std::vector<std::uint8_t> stripped;
    stripped.reserve(data.size());
    for (std::uint8_t byte : data) {
        if (byte != '\r') {
            stripped.push_back(byte);
        }
    }
    const std::string no_cr = sml_decode_text(stripped);
    if (valid_document(sml_scan_tags(no_cr)) &&
        std::none_of(out.begin(), out.end(),
                     [&](const auto& item) { return item.second == no_cr; })) {
        out.emplace_back("crlf-strip", no_cr);
    }
    return out;
}

std::pair<std::string, std::string>
sml_decode_best(std::span<const std::uint8_t> data) {
    const auto choices = sml_decode_candidates(data);
    if (choices.empty()) {
        return {sml_decode_text(data), "unrepaired"};
    }
    if (choices.front().first == "as-is") {
        return {choices.front().second, "as-is"};
    }
    // Several CRLF runs mean the file was transported with the wrong line
    // endings; one stray CR/LF is the level17 shape.
    std::size_t crlf = 0;
    for (std::size_t i = 0; i + 1 < data.size(); ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            ++crlf;
            ++i;
        }
    }
    if (crlf > 1) {
        for (const auto& choice : choices) {
            if (choice.first == "crlf-strip") {
                return {choice.second, choice.first};
            }
        }
        for (const auto& choice : choices) {
            if (choice.first == "newline-repair") {
                return {choice.second, choice.first};
            }
        }
    } else {
        for (const auto& choice : choices) {
            if (choice.first == "newline-repair") {
                return {choice.second, choice.first};
            }
        }
        for (const auto& choice : choices) {
            if (choice.first == "crlf-strip") {
                return {choice.second, choice.first};
            }
        }
    }
    return choices.front();
}

std::vector<SmlMarkupTag> sml_scan_tags(std::string_view text) {
    std::vector<SmlMarkupTag> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t lt = text.find('<', i);
        if (lt == std::string_view::npos) {
            break;
        }
        std::size_t p = lt + 1;
        SmlMarkupTag tag;
        tag.pos = lt;
        tag.closing = p < text.size() && text[p] == '/';
        if (tag.closing) {
            ++p;
        }
        if (p >= text.size() || !is_alpha(text[p])) {
            i = lt + 1;
            continue;
        }
        const std::size_t name_start = p;
        ++p;
        while (p < text.size() && is_alnum(text[p])) {
            ++p;
        }
        const std::size_t gt = text.find('>', p);
        if (gt == std::string_view::npos) {
            i = lt + 1;
            continue;
        }
        std::string_view body = text.substr(p, gt - p);
        // Python ``_TAG``: attributes must start with whitespace; ``/>`` is
        // also accepted directly after the name.  A '<' before '>' cannot be
        // part of the attribute tail.
        if (!body.empty() && body != "/" && !is_space(body.front())) {
            i = lt + 1;
            continue;
        }
        if (body.find('<') != std::string_view::npos) {
            i = lt + 1;
            continue;
        }
        tag.name.assign(text.substr(name_start, p - name_start));
        if (!body.empty() && body.back() == '/') {
            tag.self_closing = true;
            body.remove_suffix(1);
        }
        tag.raw_attrs.assign(body);
        tag.raw.assign(text.substr(lt, gt - lt + 1));
        parse_attributes(tag.raw_attrs, tag.attrs);
        out.push_back(std::move(tag));
        i = gt + 1;
    }
    return out;
}

const std::string* sml_attribute(const SmlElement& element, std::string_view name) {
    return find_attr(element.attrs, name);
}

const std::string* sml_attribute(const SmlMarkupTag& tag, std::string_view name) {
    return find_attr(tag.attrs, name);
}

std::vector<SmlElement> sml_elements(std::string_view text) {
    std::vector<SmlElement> out;
    std::vector<std::size_t> starts = {0};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    const auto line_of = [&](std::size_t pos) {
        return static_cast<std::size_t>(
            std::upper_bound(starts.begin(), starts.end(), pos) - starts.begin());
    };
    const auto add_text = [&](std::size_t pos, std::string_view body) {
        std::string collapsed = collapse_ws(body);
        if (collapsed.empty()) {
            return;
        }
        SmlElement element;
        element.kind = SmlKind::Text;
        element.pos = pos;
        element.line = line_of(pos);
        element.value = collapsed;
        element.text = std::move(collapsed);
        out.push_back(std::move(element));
    };

    std::size_t last = 0;
    for (const auto& tag : sml_scan_tags(text)) {
        add_text(last, text.substr(last, tag.pos - last));
        last = tag.pos + tag.raw.size();
        const std::string low = lower_copy(tag.name);
        SmlElement element;
        element.tag = tag.name;
        element.attrs = tag.attrs;
        element.pos = tag.pos;
        element.line = line_of(tag.pos);
        if (tag.closing) {
            element.kind = SmlKind::Close;
            element.value = tag.name;
        } else if (low == "d") {
            element.kind = SmlKind::Doc;
            element.value = tag.name;
        } else if (low == "i") {
            element.kind = SmlKind::Image;
            if (const std::string* name = find_attr(tag.attrs, "f")) {
                element.value = *name;
            }
            element.x = int_attr(tag.attrs, {"x"});
            element.y = int_attr(tag.attrs, {"y"});
            if (const std::string* align = find_attr(tag.attrs, "a")) {
                element.align = lower_char(align->empty() ? '\0' : align->front());
            }
        } else if (low == "f") {
            element.kind = SmlKind::Font;
            element.font = int_attr(tag.attrs, {"s"});
            element.value = decimal(element.font);
        } else if (low == "m") {
            element.kind = SmlKind::Music;
            element.track = int_attr(tag.attrs, {"b"});
            element.offset = int_attr(tag.attrs, {"s"});
            element.value = decimal(element.track);
        } else if (low == "r") {
            element.kind = SmlKind::Row;
            element.value = tag.name;
        } else if (low == "p") {
            element.kind = SmlKind::Paragraph;
            element.value = tag.name;
        } else if (low == "c") {
            element.kind = SmlKind::Column;
            element.value = tag.name;
        } else if (low == "k" || low == "s") {
            element.kind = SmlKind::Placeholder;
            auto it = placeholders().find(tag.name);
            element.value = it == placeholders().end() ? std::string() : it->second;
        } else {
            element.kind = SmlKind::Unknown;
            element.value = tag.name;
        }
        out.push_back(std::move(element));
    }
    add_text(last, text.substr(last));
    return out;
}

std::optional<SmlDocument> parse_sml(std::span<const std::uint8_t> data,
                                     SmlSource source, std::string& error) {
    error.clear();
    if (data.empty()) {
        error = "sml: empty input";
        return std::nullopt;
    }

    std::string text;
    std::string note;
    if (source == SmlSource::Plain) {
        text = latin1(data);
        note = "plain";
        if (const char* why = structural_error(text, sml_scan_tags(text))) {
            error = why;
            return std::nullopt;
        }
    } else {
        auto [decoded, decode_note] = sml_decode_best(data);
        const char* decoded_error = structural_error(decoded, sml_scan_tags(decoded));
        if (decoded_error == nullptr) {
            text = std::move(decoded);
            note = std::move(decode_note);
        } else if (source == SmlSource::Auto) {
            // A plain .sml does not survive the positional decode, so it has
            // to be recognised from its own bytes.
            const std::string raw = latin1(data);
            const char* raw_error = structural_error(raw, sml_scan_tags(raw));
            if (raw_error == nullptr) {
                text = raw;
                note = "plain";
            } else {
                // Report the error of the shape the bytes actually present:
                // a plain document starts with "<d" (the encoded first tag
                // letter is shifted), everything else is a failed decode.
                const std::string_view trimmed = trim_view(raw);
                const bool plain_shape =
                    trimmed.size() >= 2 && trimmed[0] == '<' &&
                    lower_char(trimmed[1]) == 'd';
                error = plain_shape ? raw_error : decoded_error;
                return std::nullopt;
            }
        } else {
            error = decoded_error;
            return std::nullopt;
        }
    }

    SmlDocument doc;
    fill_document(doc, std::move(text), std::move(note), source);
    return doc;
}

std::optional<SmlDocument> parse_sml(std::span<const std::uint8_t> data,
                                     std::string& error) {
    return parse_sml(data, SmlSource::Auto, error);
}

SmlSummary describe_sml(const SmlDocument& document) {
    SmlSummary summary;
    summary.chars = document.text.size();
    summary.lines =
        static_cast<std::size_t>(std::count(document.text.begin(), document.text.end(), '\n')) + 1;
    summary.tags = document.tags.size();
    summary.tag_limit = kSmlMaxTags;
    summary.too_many_tags = document.tags.size() >= kSmlMaxTags;
    summary.bracket_open =
        static_cast<std::size_t>(std::count(document.text.begin(), document.text.end(), '<'));
    summary.bracket_close =
        static_cast<std::size_t>(std::count(document.text.begin(), document.text.end(), '>'));
    summary.brackets_balanced = summary.bracket_open == summary.bracket_close;
    summary.doc_open = !document.tags.empty() && !document.tags.front().closing &&
                       lower_copy(document.tags.front().name) == "d";
    summary.doc_close = !document.tags.empty() && document.tags.back().closing &&
                        lower_copy(document.tags.back().name) == "d";
    summary.doc_ok = summary.doc_open && summary.doc_close;

    std::map<std::string, std::size_t> tag_counts;
    std::map<std::string, std::size_t> attributes;
    std::set<std::string> known;
    for (const auto& tag : document.tags) {
        ++tag_counts[tag.name];
        for (const auto& attr : tag.attrs) {
            ++attributes[lower_copy(tag.name) + "." + lower_copy(attr.name)];
        }
        if (!tag.closing) {
            known.insert(tag.name);
        }
    }
    summary.tag_counts.assign(tag_counts.begin(), tag_counts.end());
    summary.attributes.assign(attributes.begin(), attributes.end());
    summary.tag_letters.assign(known.begin(), known.end());
    for (const auto& name : known) {
        if (shipped_tags().count(name) == 0) {
            summary.unknown_tags.push_back(name);
        }
    }
    std::sort(summary.unknown_tags.begin(), summary.unknown_tags.end());

    for (const auto& element : document.elements) {
        switch (element.kind) {
        case SmlKind::Image: ++summary.images; break;
        case SmlKind::Font: ++summary.fonts; break;
        case SmlKind::Music: ++summary.music; break;
        case SmlKind::Text: ++summary.text_runs; break;
        case SmlKind::Placeholder: {
            const std::string low = lower_copy(element.tag);
            if (low == "k") {
                ++summary.placeholder_k;
            } else if (low == "s") {
                ++summary.placeholder_s;
            }
            break;
        }
        default: break;
        }
    }

    summary.plain = plain_of(document.text);
    summary.body = strip_tags(summary.plain);
    summary.preview = summary.body.substr(0, 400);
    return summary;
}

} // namespace gm
