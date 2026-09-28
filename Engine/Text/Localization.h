#pragma once

#include "Engine/Core/Types.h"

#include <exception>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Localization (ADR-036). Spanish is the source language: every text in the code is Spanish and is its own
// key. Other languages are gettext PO catalogs (data/lang/<code>.po) that map a source text to its
// translation. A translation whose placeholders differ from its source's (printf "%s", "%.0f"... or
// std::format "{}", "{0}", "{:.1f}"...) is rejected when the catalog loads, so a bad translation shows the
// Spanish text instead of breaking the game.
//
// The simulation never translates: what it records for the player (the journal) is a Message, a pattern and
// its arguments, rendered in the player's language when shown. Saves and state hashes do not depend on it.
namespace gx {

// Safe run-time formatting: "{}" takes the next argument, "{N}" argument N, "{{" and "}}" are braces.
// Format specs are not supported here (arguments arrive formatted). Anything malformed is copied as is.
[[nodiscard]] std::string formatPattern(std::string_view pattern, std::span<const std::string> args);

// Whether `translation` can stand for `source`: the same printf conversions in the same order, and the same
// std::format placeholders (their specs), in any order when numbered.
[[nodiscard]] bool compatiblePatterns(std::string_view source, std::string_view translation);

class Catalog {
public:
    struct Rejected {
        std::string source;
        std::string translation;
    };

    // Parses a gettext PO file: msgid/msgstr pairs, quoted strings with C escapes, continued on following
    // lines, "#" comments. The header entry (empty msgid) gives the language code ("Language: en") and its
    // name ("X-Language-Name: English"). Untranslated entries (empty msgstr) and fuzzy ones are skipped.
    bool loadPo(std::string_view text, std::string& error);

    // The translation of `source`, or nullptr.
    [[nodiscard]] const std::string* find(std::string_view source) const;
    [[nodiscard]] const char* translate(const char* source) const;
    [[nodiscard]] std::string_view translate(std::string_view source) const;

    [[nodiscard]] const std::string& language() const { return m_language; }
    [[nodiscard]] const std::string& languageName() const { return m_languageName; }
    [[nodiscard]] usize size() const { return m_entries.size(); }
    [[nodiscard]] const std::vector<Rejected>& rejected() const { return m_rejected; }

private:
    struct Hash {
        using is_transparent = void;
        usize operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
    };
    std::unordered_map<std::string, std::string, Hash, std::equal_to<>> m_entries;
    std::vector<Rejected> m_rejected;
    std::string m_language;
    std::string m_languageName;
};

// The catalog the user interface uses (nullptr: Spanish, the source). Presentation code only: the
// simulation must stay language independent.
void setActiveCatalog(const Catalog* catalog);
[[nodiscard]] const Catalog* activeCatalog();

// The source text in the active language. Marks `source` for extraction (tools/i18n.py).
[[nodiscard]] const char* tr(const char* source);
[[nodiscard]] std::string_view tr(std::string_view source);

// std::format with a translated pattern. If the translation cannot format these arguments, the source
// pattern is used; if neither can, the source text is returned unformatted.
template <typename... Args>
[[nodiscard]] std::string trf(std::string_view source, const Args&... args) {
    try {
        return std::vformat(tr(source), std::make_format_args(args...));
    } catch (const std::exception&) {
        try {
            return std::vformat(source, std::make_format_args(args...));
        } catch (const std::exception&) {
            return std::string(source);
        }
    }
}

// Marks a text for extraction without translating it (content tables): it is translated where shown.
#define GX_TEXT(text) text

// A text recorded now and rendered later, in whatever language is active then.
struct Message {
    enum class Kind : u8 {
        Pattern, // `text` is a source pattern ("{}"): translated, then filled with the rendered arguments
        Literal, // shown as is: numbers, proper names
        Term,    // a source text translated as a whole: a good, a faction
        Name,    // a proper name whose first word may be a translatable title ("Estación Arenmir IV")
    };
    Kind kind = Kind::Literal;
    std::string text;
    std::vector<Message> args;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("kind", kind);
        ar.io("text", text);
        ar.io("args", args);
    }
};

[[nodiscard]] Message literal(std::string text);
[[nodiscard]] Message term(std::string_view source);
[[nodiscard]] Message properName(std::string name);
[[nodiscard]] Message number(i64 value);
[[nodiscard]] Message number(f64 value, int decimals);

template <typename... Args>
[[nodiscard]] Message msg(std::string_view pattern, Args&&... args) {
    Message message{Message::Kind::Pattern, std::string(pattern), {}};
    message.args.reserve(sizeof...(Args));
    (message.args.push_back(std::forward<Args>(args)), ...);
    return message;
}

// Renders `message` with `catalog` (nullptr: the source language).
[[nodiscard]] std::string render(const Message& message, const Catalog* catalog);
// Every source text `message` needs translated (for coverage checks).
void collectSources(const Message& message, std::vector<std::string>& out);

} // namespace gx
