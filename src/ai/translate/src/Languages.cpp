#include <wabcut/ai/translate/Languages.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace wabcut::ai::translate {

namespace {

// Netflix adult limits (checked 2026-10-12 for en, th, ja; the rest are the guides' common European / CJK values).
constexpr std::array kLanguages{
    LanguageRules{"en", "English", "English", 20.0, 42, 2, true},
    LanguageRules{"th", "Thai", "ไทย", 17.0, 35, 2, true},
    LanguageRules{"ja", "Japanese", "日本語", 4.0, 13, 2, true},
    LanguageRules{"zh", "Chinese (Simplified)", "简体中文", 9.0, 16, 2, false},
    LanguageRules{"zh-Hant", "Chinese (Traditional)", "繁體中文", 9.0, 16, 2, false},
    LanguageRules{"ko", "Korean", "한국어", 12.0, 16, 2, false},
    LanguageRules{"vi", "Vietnamese", "Tiếng Việt", 17.0, 42, 2, false},
    LanguageRules{"id", "Indonesian", "Bahasa Indonesia", 17.0, 42, 2, false},
    LanguageRules{"ms", "Malay", "Bahasa Melayu", 17.0, 42, 2, false},
    LanguageRules{"es", "Spanish", "Español", 17.0, 42, 2, false},
    LanguageRules{"fr", "French", "Français", 17.0, 42, 2, false},
    LanguageRules{"de", "German", "Deutsch", 17.0, 42, 2, false},
    LanguageRules{"it", "Italian", "Italiano", 17.0, 42, 2, false},
    LanguageRules{"pt", "Portuguese", "Português", 17.0, 42, 2, false},
    LanguageRules{"nl", "Dutch", "Nederlands", 17.0, 42, 2, false},
    LanguageRules{"ru", "Russian", "Русский", 17.0, 42, 2, false},
    LanguageRules{"tr", "Turkish", "Türkçe", 17.0, 42, 2, false},
    LanguageRules{"ar", "Arabic", "العربية", 17.0, 42, 2, false},
    LanguageRules{"hi", "Hindi", "हिन्दी", 17.0, 42, 2, false},
    LanguageRules{"lo", "Lao", "ລາວ", 17.0, 35, 2, false},
    LanguageRules{"km", "Khmer", "ខ្មែរ", 17.0, 35, 2, false},
    LanguageRules{"my", "Burmese", "မြန်မာ", 17.0, 35, 2, false},
    LanguageRules{"fil", "Filipino", "Filipino", 17.0, 42, 2, false},
};

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

} // namespace

std::string normalizeLanguage(std::string_view code) {
    std::string primary;
    std::string subtag;
    bool inSubtag = false;
    for (const char c : code) {
        if (c == '-' || c == '_') {
            if (inSubtag) {
                break; // only one subtag is kept
            }
            inSubtag = true;
            continue;
        }
        (inSubtag ? subtag : primary).push_back(c);
    }
    std::ranges::transform(primary, primary.begin(), lower);
    if (primary == "zh" && !subtag.empty()) {
        std::string s = subtag;
        std::ranges::transform(s, s.begin(), lower);
        if (s == "hant" || s == "tw" || s == "hk" || s == "mo") {
            return "zh-Hant";
        }
        return "zh";
    }
    if (primary == "tl") {
        return "fil";
    }
    return primary;
}

LanguageRules rulesFor(std::string_view code) {
    const std::string normalized = normalizeLanguage(code);
    for (const LanguageRules& rules : kLanguages) {
        if (rules.code == normalized) {
            return rules;
        }
    }
    LanguageRules fallback;
    fallback.code = "";
    fallback.name = "";
    fallback.nativeName = "";
    return fallback;
}

std::span<const LanguageRules> knownLanguages() { return kLanguages; }

bool isLanguageTag(std::string_view code) {
    const auto dash = code.find_first_of("-_");
    const std::string_view primary = code.substr(0, dash);
    if (primary.size() < 2 || primary.size() > 3 || !std::ranges::all_of(primary, alpha)) {
        return false;
    }
    if (dash == std::string_view::npos) {
        return true;
    }
    const std::string_view subtag = code.substr(dash + 1);
    const bool letters = subtag.size() >= 2 && subtag.size() <= 4 && std::ranges::all_of(subtag, alpha);
    const bool digits = subtag.size() >= 2 && subtag.size() <= 3 && std::ranges::all_of(subtag, digit);
    return letters || digits;
}

} // namespace wabcut::ai::translate
