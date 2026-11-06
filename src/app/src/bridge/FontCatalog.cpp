#include "FontCatalog.h"

#include "ProjectController.h"

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/ProjectState.h>

#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QRawFont>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>

#include <algorithm>
#include <bit>
#include <iterator>
#include <map>
#include <set>
#include <string_view>

namespace wabcut::app::fonts {

namespace {

constexpr int kLatin = static_cast<int>(QFontDatabase::Latin);

// Qt's writing systems the picker offers (ids are QFontDatabase::WritingSystem), with what people type for them and a
// short greeting that is both the preview and the glyph check. Native names are UTF-8.
const ScriptInfo kScripts[] = {
    {QFontDatabase::Latin, "latin", "Latin", "Latin", "english western european french german spanish italian portuguese dutch polish turkish indonesian swedish en fr de es it pt nl pl tr id", "Hello"},
    {QFontDatabase::Thai, "thai", "Thai", "ไทย", "tha th siamese", "สวัสดี"},
    {QFontDatabase::Japanese, "japanese", "Japanese", "日本語", "jp ja nihongo kana kanji hiragana katakana", "こんにちは"},
    {QFontDatabase::SimplifiedChinese, "simplifiedChinese", "Chinese (Simplified)", "简体中文", "chinese 中文 mandarin zh hans cn simplified", "你好"},
    {QFontDatabase::TraditionalChinese, "traditionalChinese", "Chinese (Traditional)", "繁體中文", "chinese 中文 hant tw hk taiwan traditional cantonese", "你好嗎"},
    {QFontDatabase::Korean, "korean", "Korean", "한국어", "ko kr hangul", "안녕하세요"},
    {QFontDatabase::Arabic, "arabic", "Arabic", "العربية", "عربي ar persian farsi urdu", "مرحبا"},
    {QFontDatabase::Hebrew, "hebrew", "Hebrew", "עברית", "he iw yiddish", "שלום"},
    {QFontDatabase::Cyrillic, "cyrillic", "Cyrillic", "Кириллица", "russian русский ukrainian українська bulgarian serbian belarusian ru uk bg sr", "Привет"},
    {QFontDatabase::Greek, "greek", "Greek", "Ελληνικά", "el gr", "Γειά σου"},
    {QFontDatabase::Devanagari, "devanagari", "Devanagari", "देवनागरी", "hindi हिन्दी marathi nepali sanskrit hi mr ne", "नमस्ते"},
    {QFontDatabase::Vietnamese, "vietnamese", "Vietnamese", "Tiếng Việt", "vi viet vn", "Tiếng Việt"},
    {QFontDatabase::Armenian, "armenian", "Armenian", "Հայերեն", "hy", "Բարեւ"},
    {QFontDatabase::Georgian, "georgian", "Georgian", "ქართული", "ka", "გამარჯობა"},
    {QFontDatabase::Bengali, "bengali", "Bengali", "বাংলা", "bn bangla assamese", "নমস্কার"},
    {QFontDatabase::Tamil, "tamil", "Tamil", "தமிழ்", "ta", "வணக்கம்"},
    {QFontDatabase::Telugu, "telugu", "Telugu", "తెలుగు", "te", "నమస్కారం"},
    {QFontDatabase::Kannada, "kannada", "Kannada", "ಕನ್ನಡ", "kn", "ನಮಸ್ಕಾರ"},
    {QFontDatabase::Malayalam, "malayalam", "Malayalam", "മലയാളം", "ml", "നമസ്കാരം"},
    {QFontDatabase::Gujarati, "gujarati", "Gujarati", "ગુજરાતી", "gu", "નમસ્તે"},
    {QFontDatabase::Gurmukhi, "gurmukhi", "Punjabi (Gurmukhi)", "ਪੰਜਾਬੀ", "punjabi pa", "ਸਤ ਸ੍ਰੀ ਅਕਾਲ"},
    {QFontDatabase::Sinhala, "sinhala", "Sinhala", "සිංහල", "si", "ආයුබෝවන්"},
    {QFontDatabase::Lao, "lao", "Lao", "ລາວ", "lo", "ສະບາຍດີ"},
    {QFontDatabase::Khmer, "khmer", "Khmer", "ខ្មែរ", "km cambodian", "សួស្តី"},
    {QFontDatabase::Myanmar, "myanmar", "Burmese (Myanmar)", "မြန်မာ", "burmese my", "မင်္ဂလာပါ"},
    {QFontDatabase::Tibetan, "tibetan", "Tibetan", "བོད་ཡིག", "bo", "བཀྲ་ཤིས"},
};

bool isMarkOrLetter(QChar c) { return c.isLetterOrNumber() || c.isMark(); }

/// Folded forms of a script's names, built once.
struct ScriptWords {
    QStringList names; ///< folded english, native and every alias
    QString english;   ///< folded english
};
const std::vector<ScriptWords>& scriptWords() {
    static const std::vector<ScriptWords> words = [] {
        std::vector<ScriptWords> out;
        for (const ScriptInfo& s : kScripts) {
            ScriptWords w;
            w.english = fold(QString::fromUtf8(s.english));
            w.names << w.english << fold(QString::fromUtf8(s.native));
            for (const QString& alias : QString::fromUtf8(s.aliases).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                w.names << fold(alias);
            out.push_back(std::move(w));
        }
        return out;
    }();
    return words;
}

} // namespace

std::span<const ScriptInfo> scripts() { return kScripts; }

const ScriptInfo* scriptByKey(const QString& key) {
    for (const ScriptInfo& s : kScripts) {
        if (key == QLatin1String(s.key))
            return &s;
    }
    return nullptr;
}

const ScriptInfo* scriptById(int id) {
    for (const ScriptInfo& s : kScripts) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

QString fold(const QString& text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_D).toLower();
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        const char16_t u = c.unicode();
        if (u >= 0x0300 && u <= 0x036F)
            continue; // the accents of Latin letters
        if (isMarkOrLetter(c))
            out += c;
    }
    return out;
}

QStringList foldedWords(const QString& text) {
    QStringList out;
    for (const QString& part : text.split(QRegularExpression(QStringLiteral("[\\s\\-_.,/]+")), Qt::SkipEmptyParts)) {
        const QString folded = fold(part);
        if (!folded.isEmpty())
            out << folded;
    }
    return out;
}

int editDistance(const QString& a, const QString& b, int max) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (std::abs(n - m) > max)
        return max + 1;
    std::vector<int> previous2(static_cast<std::size_t>(m) + 1);
    std::vector<int> previous(static_cast<std::size_t>(m) + 1);
    std::vector<int> current(static_cast<std::size_t>(m) + 1);
    for (int j = 0; j <= m; ++j)
        previous[static_cast<std::size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        current[0] = i;
        int rowBest = current[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int d = std::min({previous[static_cast<std::size_t>(j)] + 1, current[static_cast<std::size_t>(j - 1)] + 1,
                              previous[static_cast<std::size_t>(j - 1)] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d = std::min(d, previous2[static_cast<std::size_t>(j - 2)] + 1); // two letters swapped
            current[static_cast<std::size_t>(j)] = d;
            rowBest = std::min(rowBest, d);
        }
        if (rowBest > max)
            return max + 1;
        previous2 = previous;
        previous = current;
    }
    return std::min(previous[static_cast<std::size_t>(m)], max + 1);
}

std::vector<int> scriptsNamed(const QString& word) {
    std::vector<int> out;
    const QString w = fold(word);
    if (w.isEmpty())
        return out;
    const auto& words = scriptWords();
    for (std::size_t i = 0; i < words.size(); ++i) {
        bool hit = words[i].names.contains(w);
        if (!hit && w.size() >= 4 && words[i].english.startsWith(w))
            hit = true; // "japan", "arab"
        if (hit)
            out.push_back(kScripts[i].id);
    }
    return out;
}

std::vector<int> scriptsOfText(const QString& text) {
    std::vector<int> out;
    const auto add = [&](QFontDatabase::WritingSystem s) {
        const int id = static_cast<int>(s);
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    };
    bool kana = false;
    bool hangul = false;
    bool han = false;
    for (const char32_t c : text.toUcs4()) {
        if ((c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
            (c >= 0x1E00 && c <= 0x1EFF)) {
            add(QFontDatabase::Latin);
            if (c >= 0x1EA0 && c <= 0x1EF9)
                add(QFontDatabase::Vietnamese); // letters stacked with two marks: Vietnamese
        } else if (c >= 0x370 && c <= 0x3FF) add(QFontDatabase::Greek);
        else if (c >= 0x400 && c <= 0x52F) add(QFontDatabase::Cyrillic);
        else if (c >= 0x530 && c <= 0x58F) add(QFontDatabase::Armenian);
        else if (c >= 0x590 && c <= 0x5FF) add(QFontDatabase::Hebrew);
        else if ((c >= 0x600 && c <= 0x6FF) || (c >= 0x750 && c <= 0x77F) || (c >= 0xFB50 && c <= 0xFDFF) ||
                 (c >= 0xFE70 && c <= 0xFEFF)) add(QFontDatabase::Arabic);
        else if (c >= 0x900 && c <= 0x97F) add(QFontDatabase::Devanagari);
        else if (c >= 0x980 && c <= 0x9FF) add(QFontDatabase::Bengali);
        else if (c >= 0xA00 && c <= 0xA7F) add(QFontDatabase::Gurmukhi);
        else if (c >= 0xA80 && c <= 0xAFF) add(QFontDatabase::Gujarati);
        else if (c >= 0xB80 && c <= 0xBFF) add(QFontDatabase::Tamil);
        else if (c >= 0xC00 && c <= 0xC7F) add(QFontDatabase::Telugu);
        else if (c >= 0xC80 && c <= 0xCFF) add(QFontDatabase::Kannada);
        else if (c >= 0xD00 && c <= 0xD7F) add(QFontDatabase::Malayalam);
        else if (c >= 0xD80 && c <= 0xDFF) add(QFontDatabase::Sinhala);
        else if (c >= 0xE00 && c <= 0xE7F) add(QFontDatabase::Thai);
        else if (c >= 0xE80 && c <= 0xEFF) add(QFontDatabase::Lao);
        else if (c >= 0xF00 && c <= 0xFFF) add(QFontDatabase::Tibetan);
        else if (c >= 0x1000 && c <= 0x109F) add(QFontDatabase::Myanmar);
        else if (c >= 0x10A0 && c <= 0x10FF) add(QFontDatabase::Georgian);
        else if (c >= 0x1780 && c <= 0x17FF) add(QFontDatabase::Khmer);
        else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) hangul = true;
        else if (c >= 0x3040 && c <= 0x30FF) kana = true;
        else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) han = true;
    }
    if (hangul)
        add(QFontDatabase::Korean);
    if (kana)
        add(QFontDatabase::Japanese);
    else if (han)
        add(QFontDatabase::SimplifiedChinese); // Han alone: a Chinese face (a Japanese one draws most of it too)
    return out;
}

int scriptOfLanguage(const QString& language) {
    const QString l = language.trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (l.isEmpty() || l == QLatin1String("auto"))
        return -1;
    const QString primary = l.section(QLatin1Char('-'), 0, 0);
    const auto in = [&](std::initializer_list<const char*> codes) {
        return std::any_of(codes.begin(), codes.end(), [&](const char* c) { return primary == QLatin1String(c); });
    };
    if (primary == QLatin1String("zh")) {
        const bool traditional = l.contains(QLatin1String("hant")) || l.endsWith(QLatin1String("-tw")) ||
                                 l.endsWith(QLatin1String("-hk")) || l.endsWith(QLatin1String("-mo"));
        return static_cast<int>(traditional ? QFontDatabase::TraditionalChinese : QFontDatabase::SimplifiedChinese);
    }
    if (l == QLatin1String("yue"))
        return static_cast<int>(QFontDatabase::TraditionalChinese);
    if (in({"th"})) return static_cast<int>(QFontDatabase::Thai);
    if (in({"ja"})) return static_cast<int>(QFontDatabase::Japanese);
    if (in({"ko"})) return static_cast<int>(QFontDatabase::Korean);
    if (in({"ar", "fa", "ur", "ps", "sd"})) return static_cast<int>(QFontDatabase::Arabic);
    if (in({"he", "iw", "yi"})) return static_cast<int>(QFontDatabase::Hebrew);
    if (in({"ru", "uk", "bg", "sr", "be", "mk", "kk", "mn", "tg"})) return static_cast<int>(QFontDatabase::Cyrillic);
    if (in({"el"})) return static_cast<int>(QFontDatabase::Greek);
    if (in({"hi", "mr", "ne", "sa"})) return static_cast<int>(QFontDatabase::Devanagari);
    if (in({"vi"})) return static_cast<int>(QFontDatabase::Vietnamese);
    if (in({"hy"})) return static_cast<int>(QFontDatabase::Armenian);
    if (in({"ka"})) return static_cast<int>(QFontDatabase::Georgian);
    if (in({"bn", "as"})) return static_cast<int>(QFontDatabase::Bengali);
    if (in({"ta"})) return static_cast<int>(QFontDatabase::Tamil);
    if (in({"te"})) return static_cast<int>(QFontDatabase::Telugu);
    if (in({"kn"})) return static_cast<int>(QFontDatabase::Kannada);
    if (in({"ml"})) return static_cast<int>(QFontDatabase::Malayalam);
    if (in({"gu"})) return static_cast<int>(QFontDatabase::Gujarati);
    if (in({"pa"})) return static_cast<int>(QFontDatabase::Gurmukhi);
    if (in({"si"})) return static_cast<int>(QFontDatabase::Sinhala);
    if (in({"lo"})) return static_cast<int>(QFontDatabase::Lao);
    if (in({"km"})) return static_cast<int>(QFontDatabase::Khmer);
    if (in({"my"})) return static_cast<int>(QFontDatabase::Myanmar);
    if (in({"bo"})) return static_cast<int>(QFontDatabase::Tibetan);
    return kLatin; // every other language Whisper and the caption tracks know is written in Latin letters
}

const char* kindKey(Kind kind) {
    switch (kind) {
    case Kind::Sans: return "sans";
    case Kind::Serif: return "serif";
    case Kind::Mono: return "mono";
    case Kind::Display: return "display";
    case Kind::Script: return "script";
    case Kind::Symbol: return "symbol";
    }
    return "sans";
}

Kind classify(const QString& family, bool fixedPitch, bool symbolOnly) {
    if (symbolOnly)
        return Kind::Symbol;
    if (fixedPitch)
        return Kind::Mono;
    const QString f = family.toLower();
    const auto has = [&](std::initializer_list<const char*> words) {
        return std::any_of(words.begin(), words.end(), [&](const char* w) { return f.contains(QLatin1String(w)); });
    };
    if (has({"wingding", "webding", "dingbat", "symbol", "emoji", "icons", "marlett"}))
        return Kind::Symbol;
    if (has({"mono", "courier", "consolas", "typewriter", "code", "terminal"}))
        return Kind::Mono;
    if (has({"script", "hand", "brush", "calligraph", "cursive", "pacifico", "lobster", "kristen", "chalk", "ink free",
             "mistral", "vivaldi", "vladimir", "edwardian", "freestyle", "gabriola", "kunstler", "corsiva", "blackadder",
             "bradley", "juice", "journal", "marker", "comic", "dancing", "sacramento", "satisfy", "caveat", "shadows"}))
        return Kind::Script;
    if (has({"display", "poster", "stencil", "impact", "haettenschweiler", "showcard", "jokerman", "broadway", "bauhaus",
             "algerian", "bernard", "britannic", "berlin sans", "castellar", "colonna", "cooper", "copperplate",
             "engravers", "forte", "gigi", "harlow", "magneto", "matura", "old english", "ravie", "playbill", "wide latin",
             "bebas", "anton", "bangers", "fredoka", "black ops", "headline"}))
        return Kind::Display;
    if (f.contains(QLatin1String("sans")) || f.contains(QLatin1String("gothic")) || f.contains(QLatin1String("grotesk")))
        return Kind::Sans;
    if (has({"serif", "times", "georgia", "garamond", "palatino", "book", "cambria", "constantia", "century", "bodoni",
             "baskerville", "didot", "minion", "caslon", "roman", "mincho", "ming", "song", "myungjo", "merriweather",
             "playfair", "lora", "cormorant", "crimson", "charter", "rockwell", "sylfaen", "perpetua", "californian",
             "bell mt", "goudy", "elephant", "bookman", "mt extra", "plantagenet", "trirong", "taviraj", "pridi"}))
        return Kind::Serif;
    return Kind::Sans;
}

namespace {

/// The scripts of a typed text that name a script, and the words of it that do not.
struct Parsed {
    QStringList words;                      ///< folded words
    std::vector<std::vector<int>> languages; ///< per word that names a script: the scripts it names (any of)
    QStringList nameWords;                   ///< the words that name no script (the family name must contain them)
};

Parsed parse(const QString& text) {
    Parsed p;
    p.words = foldedWords(text);
    // T-H-A-I: three or more single letters typed apart are one word spelled out.
    if (p.words.size() >= 3 && std::all_of(p.words.begin(), p.words.end(), [](const QString& w) { return w.size() == 1; }))
        p.words = QStringList{p.words.join(QString())};
    for (const QString& w : p.words) {
        const std::vector<int> named = scriptsNamed(w);
        if (named.empty())
            p.nameWords << w;
        else
            p.languages.push_back(named);
    }
    return p;
}

bool supportsAll(const Entry& e, const std::vector<int>& scripts) {
    return std::all_of(scripts.begin(), scripts.end(), [&](int s) { return e.hasScript(s); });
}

bool supportsAny(const Entry& e, const std::vector<int>& scripts) {
    return std::any_of(scripts.begin(), scripts.end(), [&](int s) { return e.hasScript(s); });
}

/// A name's score for the words typed: 0 for the whole name, 1 a prefix, 2 a word start, 3 inside; plus a little for length.
int nameScore(const Entry& e, const QStringList& words) {
    int worst = 0;
    for (const QString& w : words) {
        if (e.folded == w)
            continue;
        int s = 3;
        if (e.folded.startsWith(w))
            s = 1;
        else if (std::any_of(e.words.begin(), e.words.end(), [&](const QString& ew) { return ew.startsWith(w); }))
            s = 2;
        worst = std::max(worst, s);
    }
    return worst * 100 + static_cast<int>(e.folded.size());
}

bool passesFilters(const Entry& e, const Query& q) {
    if (!supportsAll(e, q.scripts) || !supportsAll(e, q.mustDraw))
        return false;
    if ((q.bold && !e.bold) || (q.italic && !e.italic) || e.weights < q.minWeights)
        return false;
    if (!q.kinds.empty() && std::find(q.kinds.begin(), q.kinds.end(), e.kind) == q.kinds.end())
        return false;
    if (q.source == QLatin1String("brand") || q.source == QLatin1String("project")) {
        if (!q.sourceFamilies.contains(e.family, Qt::CaseInsensitive))
            return false;
    } else if (q.source == QLatin1String("recent")) {
        if (!q.recent.contains(e.family))
            return false;
    } else if (q.source == QLatin1String("favourites")) {
        if (!q.favourites.contains(e.family))
            return false;
    }
    return true;
}

} // namespace

std::vector<Row> rank(const std::vector<Entry>& catalog, const Query& query) {
    std::vector<Row> rows;
    const Parsed parsed = parse(query.text);
    const bool typed = !parsed.words.isEmpty();
    // The scripts asked for by the filter or named in the text: the language sample and the "Supports" header.
    std::vector<int> asked = query.scripts;
    for (const std::vector<int>& named : parsed.languages) {
        for (const int s : named) {
            if (std::find(asked.begin(), asked.end(), s) == asked.end())
                asked.push_back(s);
        }
    }

    const auto recentRank = [&](const QString& family) {
        const qsizetype i = query.recent.indexOf(family);
        return i < 0 ? 1'000'000 : static_cast<int>(i);
    };

    if (!typed) {
        for (const Entry& e : catalog) {
            if (!passesFilters(e, query))
                continue;
            Row r;
            r.entry = &e;
            r.group = "all";
            if (query.sort == QLatin1String("relevance")) {
                if (query.favourites.contains(e.family)) {
                    r.group = "favourite";
                } else if (query.recent.indexOf(e.family) >= 0 && query.recent.indexOf(e.family) < 8) {
                    r.group = "recent";
                }
            }
            r.score = query.sort == QLatin1String("recent") ? recentRank(e.family) : 0;
            r.languageScripts = query.scripts;
            rows.push_back(std::move(r));
        }
    } else {
        std::set<const Entry*> placed;
        // 1. The name contains every word typed.
        for (const Entry& e : catalog) {
            if (!passesFilters(e, query))
                continue;
            const bool all = std::all_of(parsed.words.begin(), parsed.words.end(),
                                         [&](const QString& w) { return e.folded.contains(w); });
            if (all) {
                Row r;
                r.entry = &e;
                r.group = "name";
                r.score = nameScore(e, parsed.words);
                r.languageScripts = query.scripts;
                rows.push_back(std::move(r));
                placed.insert(&e);
            }
        }
        // 2. The font draws the language or script typed (and its other words are in the name).
        if (!parsed.languages.empty()) {
            for (const Entry& e : catalog) {
                if (placed.contains(&e) || !passesFilters(e, query))
                    continue;
                const bool languages = std::all_of(parsed.languages.begin(), parsed.languages.end(),
                                                   [&](const std::vector<int>& named) { return supportsAny(e, named); });
                const bool names = std::all_of(parsed.nameWords.begin(), parsed.nameWords.end(),
                                               [&](const QString& w) { return e.folded.contains(w); });
                if (languages && names) {
                    Row r;
                    r.entry = &e;
                    r.group = "language";
                    // Fonts made for fewer scripts first (a Thai face before one that draws everything), then short names.
                    r.score = std::popcount(e.scripts) * 10 + static_cast<int>(e.folded.size()) / 4;
                    r.languageScripts = asked;
                    rows.push_back(std::move(r));
                    placed.insert(&e);
                }
            }
        }
        // 3. Loose: the letters in order, or a small typo.
        const QString joined = parsed.words.join(QString());
        if (joined.size() >= 3) {
            const int allowed = joined.size() >= 8 ? 2 : 1;
            std::vector<Row> similar;
            for (const Entry& e : catalog) {
                if (placed.contains(&e) || !passesFilters(e, query))
                    continue;
                int best = 99;
                // A subsequence: t-h-a-i finds "Tahoma Thai Light"...
                qsizetype at = 0;
                bool subsequence = true;
                for (const QChar c : joined) {
                    at = e.folded.indexOf(c, at);
                    if (at < 0) {
                        subsequence = false;
                        break;
                    }
                    ++at;
                }
                if (subsequence)
                    best = 10 + static_cast<int>(e.folded.size() - joined.size()) / 8;
                // ... and a typo: a word of the name within one or two edits of the whole text or of one of its words.
                for (const QString& ew : e.words) {
                    const int d = std::min(editDistance(joined, ew, allowed), editDistance(joined, e.folded, allowed));
                    if (d <= allowed)
                        best = std::min(best, d * 5);
                }
                for (const QString& w : parsed.words) {
                    if (w.size() < 3)
                        continue;
                    for (const QString& ew : e.words) {
                        const int d = editDistance(w, ew, w.size() >= 8 ? 2 : 1);
                        if (d <= (w.size() >= 8 ? 2 : 1))
                            best = std::min(best, d * 5 + 3);
                    }
                }
                if (best < 99) {
                    Row r;
                    r.entry = &e;
                    r.group = "similar";
                    r.score = best;
                    r.languageScripts = query.scripts;
                    similar.push_back(std::move(r));
                }
            }
            std::sort(similar.begin(), similar.end(), [](const Row& a, const Row& b) {
                return a.score != b.score ? a.score < b.score : a.entry->family.compare(b.entry->family, Qt::CaseInsensitive) < 0;
            });
            if (similar.size() > 40)
                similar.resize(40);
            for (Row& r : similar)
                rows.push_back(std::move(r));
        }
    }

    const auto groupOrder = [](const char* g) {
        const std::string_view v(g);
        return v == "favourite" ? 0 : v == "recent" ? 1 : v == "name" ? 2 : v == "language" ? 3 : v == "similar" ? 4 : 5;
    };
    const bool alphabetical = query.sort == QLatin1String("az");
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        if (groupOrder(a.group) != groupOrder(b.group))
            return groupOrder(a.group) < groupOrder(b.group);
        if (std::string_view(a.group) == "similar")
            return false; // already in score order
        if (!alphabetical && a.score != b.score && (typed || query.sort == QLatin1String("recent")))
            return a.score < b.score;
        return a.entry->family.compare(b.entry->family, Qt::CaseInsensitive) < 0;
    });
    if (rows.size() > query.limit)
        rows.resize(query.limit);
    return rows;
}

} // namespace wabcut::app::fonts

// ---- The QML side -------------------------------------------------------------------------------------------------

namespace wabcut::app {

namespace {

constexpr int kMaxRecent = 24;

QStringList stringList(const QVariant& v) {
    QStringList out;
    for (const QVariant& item : v.toList())
        out << item.toString();
    return out;
}

/// The catalogue as the system has it. Names alone with `details` false (instant); with it the writing systems, faces
/// and glyph coverage of every family (seconds for a thousand families).
std::vector<fonts::Entry> readCatalogue(bool details, const std::atomic<std::uint64_t>* generation, std::uint64_t mine) {
    std::vector<fonts::Entry> out;
    const QStringList families = QFontDatabase::families();
    out.reserve(static_cast<std::size_t>(families.size()));
    for (const QString& family : families) {
        if (generation != nullptr && generation->load() != mine)
            return {};
        if (family.isEmpty() || family.startsWith(QLatin1Char('@')))
            continue; // the vertical-writing twins some systems list
        if (!QFontDatabase::isSmoothlyScalable(family))
            continue; // bitmap-only families: not drawable at title sizes (and DirectWrite warns on their glyphs)
        fonts::Entry e;
        e.family = family;
        e.folded = fonts::fold(family);
        e.words = fonts::foldedWords(family);
        if (e.folded.isEmpty())
            continue;
        if (!details) {
            e.kind = fonts::classify(family, false, false);
            out.push_back(std::move(e));
            continue;
        }
        e.fixedPitch = QFontDatabase::isFixedPitch(family);
        std::set<int> weights;
        for (const QString& style : QFontDatabase::styles(family)) {
            e.bold = e.bold || QFontDatabase::bold(family, style);
            e.italic = e.italic || QFontDatabase::italic(family, style);
            weights.insert(QFontDatabase::weight(family, style));
        }
        e.weights = std::max<int>(1, static_cast<int>(weights.size()));
        bool symbolOnly = false;
        const QList<QFontDatabase::WritingSystem> systems = QFontDatabase::writingSystems(family);
        for (const QFontDatabase::WritingSystem s : systems) {
            if (static_cast<int>(s) < 64)
                e.scripts |= std::uint64_t{1} << static_cast<int>(s);
        }
        symbolOnly = systems.size() == 1 && systems.front() == QFontDatabase::Symbol;
        // What the family reports can be partial (and a Thai family may leave Latin out): the real glyphs decide for every
        // script the picker offers - the family must draw every letter of the script's sample.
        const QRawFont raw = QRawFont::fromFont(QFont(family));
        if (raw.isValid() && raw.familyName().compare(family, Qt::CaseInsensitive) == 0) {
            for (const fonts::ScriptInfo& script : fonts::scripts()) {
                bool all = true;
                for (const char32_t c : QString::fromUtf8(script.sample).toUcs4()) {
                    if (c == U' ')
                        continue;
                    if (!raw.supportsCharacter(c)) {
                        all = false;
                        break;
                    }
                }
                const std::uint64_t bit = std::uint64_t{1} << script.id;
                e.scripts = all ? (e.scripts | bit) : (e.scripts & ~bit);
            }
        }
        e.kind = fonts::classify(family, e.fixedPitch, symbolOnly);
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

FontCatalogBridge::FontCatalogBridge(QObject* parent) : QObject(parent) {
    const QSettings settings;
    m_favourites = settings.value(QStringLiteral("fonts/favourites")).toStringList();
    m_recent = settings.value(QStringLiteral("fonts/recent")).toStringList();
    if (qGuiApp != nullptr) {
        m_entries = readCatalogue(false, nullptr, 0); // names now, the rest on the worker
        connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, [this] { startBuild(); });
        startBuild();
    }
}

FontCatalogBridge::~FontCatalogBridge() {
    ++m_generation; // the worker stops at its next family
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

FontCatalogBridge* FontCatalogBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new FontCatalogBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void FontCatalogBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

void FontCatalogBridge::startBuild() {
    const std::uint64_t mine = ++m_generation;
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_worker = std::jthread([this, mine] {
        std::vector<fonts::Entry> entries = readCatalogue(true, &m_generation, mine);
        if (entries.empty() || m_generation.load() != mine)
            return;
        QMetaObject::invokeMethod(this, [this, mine, entries = std::move(entries)]() mutable {
            if (m_generation.load() != mine)
                return;
            m_entries = std::move(entries);
            m_namesOnly = false;
            m_ready = true;
            {
                const std::lock_guard lock(m_glyphMutex);
                m_missing.clear();
            }
            emit readyChanged();
        }, Qt::QueuedConnection);
    });
}

void FontCatalogBridge::rebuildNow() {
    ++m_generation; // a read on the worker is dropped
    m_entries = readCatalogue(true, nullptr, 0);
    m_namesOnly = false;
    m_ready = true;
    {
        const std::lock_guard lock(m_glyphMutex);
        m_missing.clear();
    }
    emit readyChanged();
}

void FontCatalogBridge::setEntries(std::vector<fonts::Entry> entries) {
    ++m_generation;
    m_entries = std::move(entries);
    m_namesOnly = false;
    m_ready = true;
    emit readyChanged();
}

QVariantList FontCatalogBridge::scripts() const {
    QVariantList out;
    std::vector<const fonts::ScriptInfo*> sorted;
    for (const fonts::ScriptInfo& s : fonts::scripts())
        sorted.push_back(&s);
    std::stable_sort(sorted.begin(), sorted.end(), [](const fonts::ScriptInfo* a, const fonts::ScriptInfo* b) {
        // Latin first, then the scripts of the card's languages, then the rest by name.
        static const char* kFirst[] = {"latin", "thai", "japanese", "simplifiedChinese", "traditionalChinese", "korean", "arabic",
                                       "hebrew", "cyrillic", "greek", "devanagari", "vietnamese"};
        const auto rank = [](const fonts::ScriptInfo* s) {
            for (std::size_t i = 0; i < std::size(kFirst); ++i) {
                if (std::string_view(s->key) == kFirst[i])
                    return static_cast<int>(i);
            }
            return 100;
        };
        return rank(a) != rank(b) ? rank(a) < rank(b) : std::string_view(a->english) < std::string_view(b->english);
    });
    for (const fonts::ScriptInfo* s : sorted) {
        const QString english = QString::fromUtf8(s->english);
        const QString native = QString::fromUtf8(s->native);
        out.append(QVariantMap{{QStringLiteral("key"), QString::fromLatin1(s->key)},
                               {QStringLiteral("name"), english},
                               {QStringLiteral("nativeName"), native},
                               {QStringLiteral("label"), native == english ? english : english + QStringLiteral(" · ") + native},
                               {QStringLiteral("sample"), QString::fromUtf8(s->sample)}});
    }
    return out;
}

fonts::Query FontCatalogBridge::queryOf(const QString& text, const QVariantMap& filters) const {
    fonts::Query q;
    q.text = text;
    for (const QString& key : stringList(filters.value(QStringLiteral("scripts")))) {
        if (const fonts::ScriptInfo* s = fonts::scriptByKey(key))
            q.scripts.push_back(s->id);
    }
    q.bold = filters.value(QStringLiteral("bold")).toBool();
    q.italic = filters.value(QStringLiteral("italic")).toBool();
    q.minWeights = filters.value(QStringLiteral("minWeights")).toInt();
    for (const QString& key : stringList(filters.value(QStringLiteral("kinds")))) {
        for (const fonts::Kind k : {fonts::Kind::Sans, fonts::Kind::Serif, fonts::Kind::Mono, fonts::Kind::Display,
                                    fonts::Kind::Script, fonts::Kind::Symbol}) {
            if (key == QLatin1String(fonts::kindKey(k)))
                q.kinds.push_back(k);
        }
    }
    q.source = filters.value(QStringLiteral("source"), QStringLiteral("all")).toString();
    if (q.source == QLatin1String("project"))
        q.sourceFamilies = projectFamilies();
    else if (q.source == QLatin1String("brand"))
        q.sourceFamilies = stringList(filters.value(QStringLiteral("brandFamilies")));
    q.favourites = m_favourites;
    q.recent = m_recent;
    q.sort = filters.value(QStringLiteral("sort"), QStringLiteral("relevance")).toString();
    if (filters.value(QStringLiteral("hideUnsupported")).toBool())
        q.mustDraw = fonts::scriptsOfText(filters.value(QStringLiteral("text")).toString());
    return q;
}

QString FontCatalogBridge::headerFor(const fonts::Row& row) const {
    const std::string_view group(row.group);
    if (group == "favourite")
        return tr("Favourites");
    if (group == "recent")
        return tr("Recently used");
    if (group == "name")
        return tr("Name");
    if (group == "similar")
        return tr("Similar");
    if (group == "language") {
        QStringList names;
        for (const int id : row.languageScripts) {
            if (const fonts::ScriptInfo* s = fonts::scriptById(id))
                names << QString::fromUtf8(s->english);
        }
        return tr("Supports %1").arg(names.join(QStringLiteral(", ")));
    }
    return tr("All fonts");
}

QVariantMap FontCatalogBridge::search(const QString& query, const QVariantMap& filters) const {
    const fonts::Query q = queryOf(query, filters);
    const std::vector<fonts::Row> rows = fonts::rank(m_entries, q);
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const fonts::Row& row : rows) {
        // A language is asked for (typed or filtered): the font draws its greeting in that script, else its own name.
        QString sample = row.entry->family;
        if (!row.languageScripts.empty() && std::string_view(row.group) != "name" && std::string_view(row.group) != "similar") {
            if (const fonts::ScriptInfo* s = fonts::scriptById(row.languageScripts.front()))
                sample = QString::fromUtf8(s->sample);
        } else if (!q.scripts.empty() && !row.languageScripts.empty()) {
            if (const fonts::ScriptInfo* s = fonts::scriptById(q.scripts.front()))
                sample = QString::fromUtf8(s->sample);
        }
        out.append(QVariantMap{{QStringLiteral("family"), row.entry->family},
                               {QStringLiteral("group"), QString::fromLatin1(row.group)},
                               {QStringLiteral("groupLabel"), headerFor(row)},
                               {QStringLiteral("sample"), sample},
                               {QStringLiteral("kind"), QString::fromLatin1(fonts::kindKey(row.entry->kind))},
                               {QStringLiteral("favourite"), m_favourites.contains(row.entry->family)},
                               {QStringLiteral("scriptCount"), std::popcount(row.entry->scripts)}});
    }
    QString note;
    const bool needsScripts = !q.scripts.empty() || !q.mustDraw.empty() || !fonts::foldedWords(query).isEmpty();
    if (m_namesOnly && needsScripts)
        note = tr("Still reading what each font can draw: language matches appear in a moment");
    return QVariantMap{{QStringLiteral("rows"), out},
                       {QStringLiteral("total"), static_cast<int>(rows.size())},
                       {QStringLiteral("ready"), m_ready},
                       {QStringLiteral("note"), note}};
}

QString FontCatalogBridge::missingGlyphs(const QString& family, const QString& text) const {
    if (family.isEmpty() || text.trimmed().isEmpty())
        return {};
    const QString key = family + QLatin1Char('\n') + text;
    {
        const std::lock_guard lock(m_glyphMutex);
        if (const auto it = m_missing.constFind(key); it != m_missing.constEnd())
            return it.value();
    }
    QString missing;
    const QRawFont raw = QRawFont::fromFont(QFont(family));
    if (raw.isValid() && raw.familyName().compare(family, Qt::CaseInsensitive) == 0) {
        QSet<char32_t> seen;
        for (const char32_t c : text.toUcs4()) {
            if (c <= 0x20 || c == 0xA0 || !QChar::isPrint(c) || seen.contains(c))
                continue;
            seen.insert(c);
            if (!raw.supportsCharacter(c))
                missing += QString::fromUcs4(&c, 1);
        }
    }
    const std::lock_guard lock(m_glyphMutex);
    m_missing.insert(key, missing);
    return missing;
}

QStringList FontCatalogBridge::scriptsOfText(const QString& text) const {
    QStringList out;
    for (const int id : fonts::scriptsOfText(text)) {
        if (const fonts::ScriptInfo* s = fonts::scriptById(id))
            out << QString::fromLatin1(s->key);
    }
    return out;
}

QString FontCatalogBridge::scriptOfLanguage(const QString& language) const {
    const fonts::ScriptInfo* s = fonts::scriptById(fonts::scriptOfLanguage(language));
    return s ? QString::fromLatin1(s->key) : QString();
}

void FontCatalogBridge::noteUsed(const QString& family) {
    if (family.isEmpty())
        return;
    m_recent.removeAll(family);
    m_recent.prepend(family);
    while (m_recent.size() > kMaxRecent)
        m_recent.removeLast();
    QSettings().setValue(QStringLiteral("fonts/recent"), m_recent);
    emit listsChanged();
}

void FontCatalogBridge::setFavourite(const QString& family, bool on) {
    if (family.isEmpty() || m_favourites.contains(family) == on)
        return;
    if (on)
        m_favourites.append(family);
    else
        m_favourites.removeAll(family);
    QSettings().setValue(QStringLiteral("fonts/favourites"), m_favourites);
    emit listsChanged();
}

bool FontCatalogBridge::isFavourite(const QString& family) const {
    return m_favourites.contains(family);
}

bool FontCatalogBridge::isInstalled(const QString& family) const {
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [&](const fonts::Entry& e) { return e.family.compare(family, Qt::CaseInsensitive) == 0; });
}

QStringList FontCatalogBridge::projectFamilies() const {
    QStringList out;
    const auto add = [&](const std::string& family) {
        const QString f = QString::fromStdString(family);
        if (!f.isEmpty() && !out.contains(f, Qt::CaseInsensitive))
            out << f;
    };
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    for (const MediaPtr& media : snap->media) {
        if (media && media->generator)
            add(media->generator->fontFamily);
    }
    for (const SequencePtr& seq : snap->sequences) {
        if (!seq)
            continue;
        for (const CaptionTrack& track : seq->captionTracks)
            add(track.style.fontFamily);
        for (const TrackPtr& track : seq->videoTracks) {
            for (const Clip& clip : track->clips) {
                if (const Graphic* graphic = clip.graphic()) {
                    for (const GraphicLayer& layer : graphic->layers) {
                        for (const TextRun& run : layer.text.runs)
                            add(run.style.fontFamily);
                    }
                }
            }
        }
    }
    for (const CaptionPreset& preset : snap->captionPresets)
        add(preset.style.fontFamily);
    out.sort(Qt::CaseInsensitive);
    return out;
}

} // namespace wabcut::app
