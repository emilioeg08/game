#include "Engine/Text/Localization.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace gx {
namespace {

const Catalog* g_activeCatalog = nullptr;

// printf conversions in order ("%%" is not one). Stray '%' that start no conversion are ignored.
std::vector<std::string> printfConversions(std::string_view text) {
    std::vector<std::string> out;
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '%') {
            ++i;
            continue;
        }
        usize j = i + 1;
        while (j < text.size() && std::string_view("-+ #0").find(text[j]) != std::string_view::npos) {
            ++j;
        }
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '*')) {
            ++j;
        }
        if (j < text.size() && text[j] == '.') {
            ++j;
            while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '*')) {
                ++j;
            }
        }
        while (j < text.size() && std::string_view("hljztL").find(text[j]) != std::string_view::npos) {
            ++j;
        }
        if (j < text.size() &&
            std::string_view("diouxXeEfFgGaAcsp").find(text[j]) != std::string_view::npos) {
            out.emplace_back(text.substr(i, j - i + 1));
            i = j;
        }
    }
    return out;
}

struct BracePlaceholder {
    int index = -1; // -1: automatic
    std::string spec;
    auto operator<=>(const BracePlaceholder&) const = default;
};

// std::format placeholders; false if malformed or mixing automatic and manual numbering.
bool bracePlaceholders(std::string_view text, std::vector<BracePlaceholder>& out) {
    bool automatic = false;
    bool manual = false;
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                ++i;
                continue;
            }
            return false;
        }
        if (text[i] != '{') {
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        const usize close = text.find('}', i);
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view inside = text.substr(i + 1, close - i - 1);
        const usize colon = inside.find(':');
        const std::string_view id = inside.substr(0, colon);
        BracePlaceholder placeholder;
        if (colon != std::string_view::npos) {
            placeholder.spec = std::string(inside.substr(colon));
        }
        if (id.empty()) {
            automatic = true;
        } else {
            int index = 0;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), index);
            if (error != std::errc{} || end != id.data() + id.size()) {
                return false;
            }
            placeholder.index = index;
            manual = true;
        }
        out.push_back(std::move(placeholder));
        i = close;
    }
    if (automatic && manual) {
        return false;
    }
    if (automatic) {
        for (usize k = 0; k < out.size(); ++k) {
            out[k].index = static_cast<int>(k);
        }
    }
    std::sort(out.begin(), out.end());
    return true;
}

// Unquotes a PO string literal ("..." with C escapes). False if malformed.
bool unquote(std::string_view text, std::string& out) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return false;
    }
    text = text.substr(1, text.size() - 2);
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
            if (text[i] == '"') {
                return false;
            }
            out += text[i];
            continue;
        }
        if (++i >= text.size()) {
            return false;
        }
        switch (text[i]) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '"':
        case '\\':
            out += text[i];
            break;
        default:
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// The value of `key` in a PO header ("Language: en\n...").
std::string headerField(std::string_view header, std::string_view key) {
    while (!header.empty()) {
        const auto end = header.find('\n');
        const std::string_view line = header.substr(0, end);
        header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 1);
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ':') {
            return std::string(trim(line.substr(key.size() + 1)));
        }
    }
    return {};
}

} // namespace

std::string formatPattern(std::string_view pattern, std::span<const std::string> args) {
    std::string out;
    out.reserve(pattern.size() + 16);
    usize next = 0;
    for (usize i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c) {
            out += c;
            ++i;
            continue;
        }
        if (c != '{') {
            out += c;
            continue;
        }
        const usize close = pattern.find('}', i);
        if (close == std::string_view::npos) {
            out += pattern.substr(i);
            break;
        }
        const std::string_view id = pattern.substr(i + 1, close - i - 1);
        usize index = next;
        bool valid = true;
        if (id.empty()) {
            ++next;
        } else {
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), index);
            valid = error == std::errc{} && end == id.data() + id.size();
        }
        if (valid && index < args.size()) {
            out += args[index];
        } else {
            out += pattern.substr(i, close - i + 1);
        }
        i = close;
    }
    return out;
}

bool compatiblePatterns(std::string_view source, std::string_view translation) {
    if (printfConversions(source) != printfConversions(translation)) {
        return false;
    }
    std::vector<BracePlaceholder> a;
    std::vector<BracePlaceholder> b;
    const bool sourceValid = bracePlaceholders(source, a);
    const bool translationValid = bracePlaceholders(translation, b);
    return !sourceValid || (translationValid && a == b); // a source that is no pattern: only printf matters
}

bool Catalog::loadPo(std::string_view text, std::string& error) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3); // UTF-8 byte order mark
    }
    std::string id;
    std::string str;
    std::string* field = nullptr;
    bool inEntry = false;
    bool fuzzy = false;
    bool plural = false;
    const auto flush = [&] {
        if (inEntry && !plural) {
            if (id.empty()) {
                m_language = headerField(str, "Language");
                m_languageName = headerField(str, "X-Language-Name");
            } else if (!fuzzy && !str.empty()) {
                if (compatiblePatterns(id, str)) {
                    m_entries.insert_or_assign(id, str);
                } else {
                    m_rejected.push_back({id, str});
                }
            }
        }
        id.clear();
        str.clear();
        field = nullptr;
        inEntry = false;
        fuzzy = false;
        plural = false;
    };
    usize lineNumber = 0;
    while (!text.empty()) {
        ++lineNumber;
        const auto end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto fail = [&](std::string_view what) {
            error = std::format("line {}: {}", lineNumber, what);
            return false;
        };
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            if (line.starts_with("#,") && line.find("fuzzy") != std::string_view::npos) {
                flush();
                fuzzy = true; // flags come before the entry they describe
            }
            continue;
        }
        std::string_view value;
        if (line.starts_with("msgctxt ")) {
            continue; // contexts are not used: the source text is the key
        }
        if (line.starts_with("msgid_plural ") || line.starts_with("msgstr[")) {
            plural = true; // plural forms are not used
            field = nullptr;
            continue;
        }
        if (line.starts_with("msgid ")) {
            const bool keepFuzzy = fuzzy && !inEntry;
            flush();
            fuzzy = keepFuzzy;
            inEntry = true;
            field = &id;
            value = trim(line.substr(6));
        } else if (line.starts_with("msgstr ")) {
            if (!inEntry) {
                return fail("msgstr without msgid");
            }
            field = &str;
            value = trim(line.substr(7));
        } else if (line.front() == '"') {
            if (field == nullptr) {
                if (plural) {
                    continue;
                }
                return fail("string outside an entry");
            }
            value = line;
        } else {
            return fail("unexpected text");
        }
        if (field != nullptr && !unquote(value, *field)) {
            return fail("malformed string");
        }
    }
    flush();
    return true;
}

const std::string* Catalog::find(std::string_view source) const {
    const auto it = m_entries.find(source);
    return it != m_entries.end() ? &it->second : nullptr;
}

const char* Catalog::translate(const char* source) const {
    const std::string* translation = find(source);
    return translation != nullptr ? translation->c_str() : source;
}

std::string_view Catalog::translate(std::string_view source) const {
    const std::string* translation = find(source);
    return translation != nullptr ? std::string_view(*translation) : source;
}

void setActiveCatalog(const Catalog* catalog) {
    g_activeCatalog = catalog;
}

const Catalog* activeCatalog() {
    return g_activeCatalog;
}

const char* tr(const char* source) {
    return g_activeCatalog != nullptr ? g_activeCatalog->translate(source) : source;
}

std::string_view tr(std::string_view source) {
    return g_activeCatalog != nullptr ? g_activeCatalog->translate(source) : source;
}

Message literal(std::string text) {
    return {Message::Kind::Literal, std::move(text), {}};
}

Message term(std::string_view source) {
    return {Message::Kind::Term, std::string(source), {}};
}

Message properName(std::string name) {
    return {Message::Kind::Name, std::move(name), {}};
}

Message number(i64 value) {
    return literal(std::to_string(value));
}

Message number(f64 value, int decimals) {
    return literal(std::format("{:.{}f}", value, decimals));
}

std::string render(const Message& message, const Catalog* catalog) {
    switch (message.kind) {
    case Message::Kind::Literal:
        return message.text;
    case Message::Kind::Term:
        return std::string(catalog != nullptr ? catalog->translate(std::string_view(message.text))
                                              : std::string_view(message.text));
    case Message::Kind::Name: {
        const usize space = message.text.find(' ');
        const std::string* title = catalog != nullptr && space != std::string::npos
                                       ? catalog->find(message.text.substr(0, space))
                                       : nullptr;
        return title != nullptr ? *title + message.text.substr(space) : message.text;
    }
    case Message::Kind::Pattern: {
        std::vector<std::string> args;
        args.reserve(message.args.size());
        for (const Message& arg : message.args) {
            args.push_back(render(arg, catalog));
        }
        const std::string_view pattern =
            catalog != nullptr ? catalog->translate(std::string_view(message.text)) : message.text;
        return formatPattern(pattern, args);
    }
    }
    return message.text;
}

void collectSources(const Message& message, std::vector<std::string>& out) {
    if (message.kind == Message::Kind::Pattern || message.kind == Message::Kind::Term) {
        out.push_back(message.text);
    }
    for (const Message& arg : message.args) {
        collectSources(arg, out);
    }
}

} // namespace gx
