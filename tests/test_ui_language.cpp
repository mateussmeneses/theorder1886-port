// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu's language tables (gpu/shim/port_ui_language.h): every language has exactly
// the keys of port_ui_keys.h, each translation keeps the printf conversions of its English text
// (a missing or extra %d would read the wrong argument), and the lookup follows the language.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "port_ui_language.h"
#include "ui_language/port_ui_keys.h"

// The printf conversions of a text, in order ("%d", "%u", "%%", "%s"...). Help texts such as
// "100% shows" are not formats: a '%' before a space or a letter-less end is plain text.
static std::vector<std::string> Conversions(std::string_view text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        std::size_t j = i + 1;
        while (j < text.size() && std::strchr("0123456789.l", text[j])) ++j;
        if (j < text.size() && !std::strchr("%dusfx", text[j])) continue;
        if (j < text.size()) out.emplace_back(text.substr(i, j - i + 1));
        i = j;
    }
    return out;
}

int main() {
    constexpr std::size_t key_count = sizeof(PortUi::Keys) / sizeof(PortUi::Keys[0]);
    for (int l = 1; l < PortUi::LanguageCount; ++l) {
        const auto& language = PortUi::Languages[l];
        if (language.count != key_count) {
            std::fprintf(stderr, "%.*s: %zu texts, %zu keys\n", int(language.code.size()), language.code.data(),
                         language.count, key_count);
        }
        assert(language.count == key_count);
        for (const char* key : PortUi::Keys) {
            const char* text = PortUi::Lookup(language, key);
            bool found = false;
            for (std::size_t i = 0; i < language.count; ++i) found |= language.texts[i].first == key;
            if (!found || Conversions(key) != Conversions(text)) {
                std::fprintf(stderr, "%.*s: \"%s\" %s\n", int(language.code.size()), language.code.data(), key,
                             found ? "changes its printf conversions" : "is missing");
            }
            assert(found);
            assert(Conversions(key) == Conversions(text));
            assert(text[0] != '\0');
        }
    }
    assert(PortUi::FindLanguage("pt-BR") == 1 && PortUi::FindLanguage("de_DE") == 4);
    assert(PortUi::FindLanguage("en") == 0 && PortUi::FindLanguage("ja") == 0 && PortUi::FindLanguage("") == 0);
    assert(PortUi::SetLanguage(1) == 1);
    assert(std::string_view(PortUi::Tr("Close")) == "Fechar");
    assert(std::string_view(PortUi::Tr("R2")) == "R2"); // not a key: stays as it is
    assert(PortUi::SetLanguage(99) == 0);
    assert(std::string_view(PortUi::Tr("Close")) == "Close");
    assert(PortUi::Tr(nullptr) == nullptr);
    std::puts("ui-language-test: ok");
    return 0;
}
