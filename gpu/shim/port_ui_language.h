// SPDX-License-Identifier: GPL-2.0-or-later
// port (after SpartanPort's spartanport_ui_language.h): the in-game menu's languages. Every table
// is keyed by the English text, and Tr() looks a text up in the language in use; English needs
// no table, and a text missing from a table stays English. The embedded DejaVu Sans draws them.
#pragma once
#include <atomic>
#include <cstddef>
#include <string_view>
#include <utility>

#include "ui_language/port_ui_de.h"
#include "ui_language/port_ui_es.h"
#include "ui_language/port_ui_fr.h"
#include "ui_language/port_ui_it.h"
#include "ui_language/port_ui_pt.h"

namespace PortUi {
using TextTable = std::pair<std::string_view, const char*>;

struct Language {
    std::string_view code; // ISO 639-1, as theorder.ini ui_language names it
    const char* name;      // in the language itself
    const TextTable* texts;
    std::size_t count;
};
template <std::size_t N>
constexpr Language Table(std::string_view code, const char* name, const TextTable (&texts)[N]) {
    return {code, name, texts, N};
}
/// The menu's languages, in the order of the language list; 0 is English.
inline constexpr Language Languages[] = {
    {"en", "English", nullptr, 0},
    Table("pt", "Português (Brasil)", PortugueseText),
    Table("es", "Español", SpanishText),
    Table("fr", "Français", FrenchText),
    Table("de", "Deutsch", GermanText),
    Table("it", "Italiano", ItalianText),
};
inline constexpr int LanguageCount = int(sizeof(Languages) / sizeof(Languages[0]));
inline std::atomic<int> language{0}; // index into Languages

/// The index of a language code ("pt", "pt-BR", "DE_de"...; any case); 0 (English) for any other.
inline int FindLanguage(std::string_view code) {
    char two[2] = {};
    for (std::size_t i = 0; i < 2 && i < code.size(); ++i) {
        const char c = code[i];
        two[i] = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    }
    for (int i = 1; i < LanguageCount; ++i) {
        if (Languages[i].code == std::string_view(two, 2)) return i;
    }
    return 0;
}

/// Uses the language at once. Returns the index in use.
inline int SetLanguage(int index) {
    if (index < 0 || index >= LanguageCount) index = 0;
    language = index;
    return index;
}

inline const char* Lookup(const Language& table, const char* text) {
    const std::string_view key(text); // measured once, not once per entry
    for (std::size_t i = 0; i < table.count; ++i) {
        if (table.texts[i].first == key) return table.texts[i].second;
    }
    return text;
}

inline const char* Tr(const char* text) {
    if (!text) return text;
    const int index = language.load(std::memory_order_relaxed);
    return index > 0 && index < LanguageCount ? Lookup(Languages[index], text) : text;
}
} // namespace PortUi
