#include <wabcut/ai/SpokenLanguages.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace wabcut::ai {

namespace {

constexpr SpokenTier S = SpokenTier::Strong;
constexpr SpokenTier F = SpokenTier::Fair;
constexpr SpokenTier L = SpokenTier::Limited;

// Whisper's language table (whisper.cpp g_lang, large-v3: 99 languages and Cantonese). Native names are written in their
// own script; the tier is the amount of training audio (SpokenLanguages.h).
constexpr std::array kLanguages{
    SpokenLanguage{"en", "English", "English", false, S},
    SpokenLanguage{"zh", "Chinese", "中文", false, S},
    SpokenLanguage{"de", "German", "Deutsch", false, S},
    SpokenLanguage{"es", "Spanish", "Español", false, S},
    SpokenLanguage{"ru", "Russian", "Русский", false, S},
    SpokenLanguage{"ko", "Korean", "한국어", false, S},
    SpokenLanguage{"fr", "French", "Français", false, S},
    SpokenLanguage{"ja", "Japanese", "日本語", false, S},
    SpokenLanguage{"pt", "Portuguese", "Português", false, S},
    SpokenLanguage{"tr", "Turkish", "Türkçe", false, S},
    SpokenLanguage{"pl", "Polish", "Polski", false, S},
    SpokenLanguage{"ca", "Catalan", "Català", false, S},
    SpokenLanguage{"nl", "Dutch", "Nederlands", false, S},
    SpokenLanguage{"ar", "Arabic", "العربية", true, S},
    SpokenLanguage{"sv", "Swedish", "Svenska", false, S},
    SpokenLanguage{"it", "Italian", "Italiano", false, S},
    SpokenLanguage{"id", "Indonesian", "Bahasa Indonesia", false, S},
    SpokenLanguage{"hi", "Hindi", "हिन्दी", false, L},
    SpokenLanguage{"fi", "Finnish", "Suomi", false, S},
    SpokenLanguage{"vi", "Vietnamese", "Tiếng Việt", false, S},
    SpokenLanguage{"he", "Hebrew", "עברית", true, S},
    SpokenLanguage{"uk", "Ukrainian", "Українська", false, S},
    SpokenLanguage{"el", "Greek", "Ελληνικά", false, F},
    SpokenLanguage{"ms", "Malay", "Bahasa Melayu", false, F},
    SpokenLanguage{"cs", "Czech", "Čeština", false, F},
    SpokenLanguage{"ro", "Romanian", "Română", false, F},
    SpokenLanguage{"da", "Danish", "Dansk", false, F},
    SpokenLanguage{"hu", "Hungarian", "Magyar", false, F},
    SpokenLanguage{"ta", "Tamil", "தமிழ்", false, F},
    SpokenLanguage{"no", "Norwegian", "Norsk", false, F},
    SpokenLanguage{"th", "Thai", "ไทย", false, F},
    SpokenLanguage{"ur", "Urdu", "اردو", true, F},
    SpokenLanguage{"hr", "Croatian", "Hrvatski", false, F},
    SpokenLanguage{"bg", "Bulgarian", "Български", false, L},
    SpokenLanguage{"lt", "Lithuanian", "Lietuvių", false, L},
    SpokenLanguage{"la", "Latin", "Latina", false, L},
    SpokenLanguage{"mi", "Maori", "Māori", false, L},
    SpokenLanguage{"ml", "Malayalam", "മലയാളം", false, L},
    SpokenLanguage{"cy", "Welsh", "Cymraeg", false, L},
    SpokenLanguage{"sk", "Slovak", "Slovenčina", false, L},
    SpokenLanguage{"te", "Telugu", "తెలుగు", false, L},
    SpokenLanguage{"fa", "Persian", "فارسی", true, L},
    SpokenLanguage{"lv", "Latvian", "Latviešu", false, L},
    SpokenLanguage{"bn", "Bengali", "বাংলা", false, L},
    SpokenLanguage{"sr", "Serbian", "Српски", false, L},
    SpokenLanguage{"az", "Azerbaijani", "Azərbaycan", false, L},
    SpokenLanguage{"sl", "Slovenian", "Slovenščina", false, L},
    SpokenLanguage{"kn", "Kannada", "ಕನ್ನಡ", false, L},
    SpokenLanguage{"et", "Estonian", "Eesti", false, L},
    SpokenLanguage{"mk", "Macedonian", "Македонски", false, L},
    SpokenLanguage{"br", "Breton", "Brezhoneg", false, L},
    SpokenLanguage{"eu", "Basque", "Euskara", false, L},
    SpokenLanguage{"is", "Icelandic", "Íslenska", false, L},
    SpokenLanguage{"hy", "Armenian", "Հայերեն", false, L},
    SpokenLanguage{"ne", "Nepali", "नेपाली", false, L},
    SpokenLanguage{"mn", "Mongolian", "Монгол", false, L},
    SpokenLanguage{"bs", "Bosnian", "Bosanski", false, L},
    SpokenLanguage{"kk", "Kazakh", "Қазақша", false, L},
    SpokenLanguage{"sq", "Albanian", "Shqip", false, L},
    SpokenLanguage{"sw", "Swahili", "Kiswahili", false, L},
    SpokenLanguage{"gl", "Galician", "Galego", false, L},
    SpokenLanguage{"mr", "Marathi", "मराठी", false, L},
    SpokenLanguage{"pa", "Punjabi", "ਪੰਜਾਬੀ", false, L},
    SpokenLanguage{"si", "Sinhala", "සිංහල", false, L},
    SpokenLanguage{"km", "Khmer", "ខ្មែរ", false, L},
    SpokenLanguage{"sn", "Shona", "chiShona", false, L},
    SpokenLanguage{"yo", "Yoruba", "Yorùbá", false, L},
    SpokenLanguage{"so", "Somali", "Soomaali", false, L},
    SpokenLanguage{"af", "Afrikaans", "Afrikaans", false, L},
    SpokenLanguage{"oc", "Occitan", "Occitan", false, L},
    SpokenLanguage{"ka", "Georgian", "ქართული", false, L},
    SpokenLanguage{"be", "Belarusian", "Беларуская", false, L},
    SpokenLanguage{"tg", "Tajik", "Тоҷикӣ", false, L},
    SpokenLanguage{"sd", "Sindhi", "سنڌي", true, L},
    SpokenLanguage{"gu", "Gujarati", "ગુજરાતી", false, L},
    SpokenLanguage{"am", "Amharic", "አማርኛ", false, L},
    SpokenLanguage{"yi", "Yiddish", "ייִדיש", true, L},
    SpokenLanguage{"lo", "Lao", "ລາວ", false, L},
    SpokenLanguage{"uz", "Uzbek", "Oʻzbek", false, L},
    SpokenLanguage{"fo", "Faroese", "Føroyskt", false, L},
    SpokenLanguage{"ht", "Haitian Creole", "Kreyòl ayisyen", false, L},
    SpokenLanguage{"ps", "Pashto", "پښتو", true, L},
    SpokenLanguage{"tk", "Turkmen", "Türkmen", false, L},
    SpokenLanguage{"nn", "Nynorsk", "Nynorsk", false, L},
    SpokenLanguage{"mt", "Maltese", "Malti", false, L},
    SpokenLanguage{"sa", "Sanskrit", "संस्कृतम्", false, L},
    SpokenLanguage{"lb", "Luxembourgish", "Lëtzebuergesch", false, L},
    SpokenLanguage{"my", "Burmese", "မြန်မာ", false, L},
    SpokenLanguage{"bo", "Tibetan", "བོད་སྐད་", false, L},
    SpokenLanguage{"tl", "Tagalog", "Tagalog", false, L},
    SpokenLanguage{"mg", "Malagasy", "Malagasy", false, L},
    SpokenLanguage{"as", "Assamese", "অসমীয়া", false, L},
    SpokenLanguage{"tt", "Tatar", "Татарча", false, L},
    SpokenLanguage{"haw", "Hawaiian", "ʻŌlelo Hawaiʻi", false, L},
    SpokenLanguage{"ln", "Lingala", "Lingála", false, L},
    SpokenLanguage{"ha", "Hausa", "Hausa", false, L},
    SpokenLanguage{"ba", "Bashkir", "Башҡортса", false, L},
    SpokenLanguage{"jw", "Javanese", "Basa Jawa", false, L},
    SpokenLanguage{"su", "Sundanese", "Basa Sunda", false, L},
    SpokenLanguage{"yue", "Cantonese", "粵語", false, L},
};

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

} // namespace

std::span<const SpokenLanguage> spokenLanguages() { return kLanguages; }

const SpokenLanguage* findSpokenLanguage(std::string_view code) {
    // The primary subtag, lower case: "he-IL" and "HE" are Hebrew.
    std::string primary;
    for (const char c : code) {
        if (c == '-' || c == '_') {
            break;
        }
        primary.push_back(lower(c));
    }
    if (primary == "iw") {
        primary = "he"; // the old code for Hebrew
    } else if (primary == "nb") {
        primary = "no"; // Norwegian Bokmal
    } else if (primary == "fil") {
        primary = "tl"; // Filipino
    } else if (primary == "jv") {
        primary = "jw"; // whisper still says jw for Javanese
    }
    for (const SpokenLanguage& language : kLanguages) {
        if (language.code == primary) {
            return &language;
        }
    }
    return nullptr;
}

bool isSpokenLanguageChoice(std::string_view code) {
    return code == "auto" || findSpokenLanguage(code) != nullptr;
}

std::string spokenLanguageCode(std::string_view code) {
    if (code == "auto") {
        return "auto";
    }
    const SpokenLanguage* language = findSpokenLanguage(code);
    return language != nullptr ? std::string(language->code) : std::string();
}

bool isRightToLeftLanguage(std::string_view code) {
    const SpokenLanguage* language = findSpokenLanguage(code);
    return language != nullptr && language->rtl;
}

std::string spokenLanguageAdvice(std::string_view code, std::string_view modelId) {
    const SpokenLanguage* language = findSpokenLanguage(code);
    if (language == nullptr) {
        return {};
    }
    const std::string name(language->name);
    const bool english = contains(modelId, ".en");
    if (english && language->code != "en") {
        return "The " + std::string(modelId) + " model only knows English: pick a multilingual model in Settings > AI > Local models.";
    }
    const bool tiny = modelId == "tiny";
    const bool small = modelId == "base" || modelId == "tiny" || modelId == "base.en";
    const bool middling = small || modelId == "small" || modelId == "small.en";
    switch (language->tier) {
    case SpokenTier::Strong:
        if (tiny) {
            return "The tiny model is for tests: pick Base or larger for " + name + ".";
        }
        return {};
    case SpokenTier::Fair:
        if (small) {
            return name + " is a language Whisper heard less of: Small or larger (Settings > AI > Local models) gives dependable text.";
        }
        return {};
    case SpokenTier::Limited:
        if (middling) {
            return "Whisper heard little " + name + " audio in training: Medium or Large v3 (Settings > AI > Local models) is far more accurate, and check the result either way.";
        }
        return "Whisper heard little " + name + " audio in training: read the result through.";
    }
    return {};
}

bool modelCanTranslate(std::string_view modelId) { return !contains(modelId, ".en"); }

std::string translationAdvice(std::string_view modelId) {
    if (contains(modelId, ".en")) {
        return "English-only models cannot translate: pick a multilingual model in Settings > AI > Local models.";
    }
    if (contains(modelId, "turbo")) {
        return "Whisper large-v3-turbo is built for transcription and translates poorly: pick Small, Medium or Large v3 in Settings > AI > Local models.";
    }
    if (modelId == "tiny" || modelId == "base") {
        return "The tiny and base models translate roughly: Small or larger (Settings > AI > Local models) is much better.";
    }
    return {};
}

std::string mixedLanguagesAdvice(std::string_view modelId) {
    if (contains(modelId, ".en")) {
        return "English-only models cannot tell languages apart: pick a multilingual model in Settings > AI > Local models.";
    }
    if (modelId == "tiny" || modelId == "base") {
        return "The tiny and base models often name the wrong language for a short stretch: Small or larger (Settings > AI > Local models) tells them apart far better. Each stretch is listened to separately, so this takes longer.";
    }
    return "Each stretch of speech is listened to on its own, so this takes longer than one language.";
}

} // namespace wabcut::ai
