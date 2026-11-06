#pragma once

// FontCatalog (card 053): the one font search every font choice shares - Effect Controls' Text section, the
// Essential Graphics editor, the Brand Kit and the caption styles (qml/components/FontPicker.qml over the `Fonts`
// singleton).
//
//   * Search: one box. Typing "Thai" lists, in groups with a header each, (1) the fonts whose name matches ("Noto Sans
//     Thai", "Leelawadee UI Thai"), (2) the fonts that can draw the language or script typed ("Supports Thai": Tahoma,
//     Leelawadee UI...), (3) loose matches: letters in order, spaces and dashes ignored, small typos ("Similar"). Language
//     and script names work in English and in their own script (Thai / ไทย, Japanese / 日本語, Arabic / عربي, ...); "thai
//     sans" is the script plus a name word. Each row is drawn in its own font, with a sample in the script when a language
//     is asked for.
//   * Filters (all combine): language or script (a font must draw every one chosen), has Bold / has Italic / several
//     weights, type (Sans, Serif, Mono, Display, Handwriting, Symbol), source (Brand kit, Recently used, Favourites, Project
//     fonts), sort (relevance, A-Z, recently used), and hiding the fonts that cannot draw the text a layer has.
//   * What a font can draw comes from Qt's writing systems for the family and, for the scripts people look for, from the
//     real glyphs: a short sample of the script is checked against the font, because a family's report can be partial. The
//     catalogue is read once per session on a worker thread (the picker shows names meanwhile) and again when the system's
//     fonts change.
//
// The ranking (`fonts::rank`) is pure over `fonts::Entry` lists, so it is tested without any font; FontCatalogBridge is the
// QML side (favourites and recent fonts are remembered in QSettings).

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {
class ProjectController;
}

namespace wabcut::app::fonts {

/// A script (a writing system) a font can draw, with the words people type for it.
struct ScriptInfo {
    int id;               ///< QFontDatabase::WritingSystem; ids are below 64 (the bit in Entry::scripts)
    const char* key;      ///< "thai"
    const char* english;  ///< "Thai"
    const char* native;   ///< the language's own name, UTF-8 ("ไทย")
    const char* aliases;  ///< more words, lower case, space separated: languages and codes that use the script
    const char* sample;   ///< a short greeting in it, UTF-8: the preview text and the glyph check
};

std::span<const ScriptInfo> scripts();
const ScriptInfo* scriptByKey(const QString& key);
const ScriptInfo* scriptById(int id);
/// The scripts a typed word names: its English or own name, an alias or code, or - from four letters - the start of the
/// English name ("japan"). Empty when it names none.
std::vector<int> scriptsNamed(const QString& word);
/// The scripts the characters of `text` need (Latin letters, Thai, Hebrew, ...; digits, spaces and punctuation need none).
std::vector<int> scriptsOfText(const QString& text);
/// The script a language code (a caption track's, "th", "ja", "zh-Hant", "ar") is written in, or -1.
int scriptOfLanguage(const QString& language);

enum class Kind : std::uint8_t { Sans, Serif, Mono, Display, Script, Symbol };
const char* kindKey(Kind kind);
/// The type of a family from its name and what Qt says of it: fixed pitch is Mono, a symbol-only family is Symbol, then
/// words in the name ("Serif", "Script", "Display", ...); everything else is Sans.
Kind classify(const QString& family, bool fixedPitch, bool symbolOnly);

/// What the picker knows about one installed family.
struct Entry {
    QString family;
    QString folded;          ///< lower case, letters and digits only: "notosansthai"
    QStringList words;       ///< the folded words of the name: "noto", "sans", "thai"
    std::uint64_t scripts = 0; ///< bit i: draws script i (Qt's report, replaced by the glyph check for the probed ones)
    bool fixedPitch = false;
    bool bold = false;
    bool italic = false;
    int weights = 1;         ///< distinct weights the family has
    Kind kind = Kind::Sans;
    bool hasScript(int id) const { return id >= 0 && id < 64 && ((scripts >> id) & 1u) != 0; }
};

/// Lower case, accents of Latin letters dropped, everything but letters, digits and marks removed: how names and typed
/// words are compared ("Noto Sans-Thai" and "noto sans thai" both fold to "notosansthai").
QString fold(const QString& text);
/// The folded words of a typed text, split at spaces, dashes, underscores, dots and commas.
QStringList foldedWords(const QString& text);
/// Damerau-Levenshtein distance of two strings, or max + 1 when it is above `max` (an insertion, a deletion, a changed
/// letter or two swapped letters each cost one).
int editDistance(const QString& a, const QString& b, int max);

struct Query {
    QString text;                    ///< what was typed
    std::vector<int> scripts;        ///< the language / script filter: the font must draw every one
    bool bold = false;               ///< the family has a bold face
    bool italic = false;             ///< and an italic one
    int minWeights = 0;              ///< at least this many weights
    std::vector<Kind> kinds;         ///< only these types (empty: any)
    QString source = QStringLiteral("all"); ///< "all", "brand", "recent", "favourites" or "project"
    QStringList sourceFamilies;      ///< the families of the brand kit / project when source says so
    QStringList favourites;          ///< starred families
    QStringList recent;              ///< recently used families, the latest first
    QString sort = QStringLiteral("relevance"); ///< "relevance", "az" or "recent"
    std::vector<int> mustDraw;       ///< scripts the layer's text needs: fonts that cannot draw one are left out
    std::size_t limit = 400;         ///< the most rows returned
};

struct Row {
    const Entry* entry = nullptr;
    /// "favourite", "recent" or "all" (nothing typed); "name", "language" or "similar" (typed text).
    const char* group = "all";
    int score = 0;
    /// The scripts of the text and the filter that made the row a language match (for its sample and header).
    std::vector<int> languageScripts;
};

/// The rows for `query` over `catalog`, in the order the picker shows them: grouped, then by score, then by name.
std::vector<Row> rank(const std::vector<Entry>& catalog, const Query& query);

} // namespace wabcut::app::fonts

namespace wabcut::app {

class FontCatalogBridge : public QObject {
    Q_OBJECT
    /// The catalogue has been read (writing systems and glyph coverage): script filters work. Names work before.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged FINAL)
    /// How many families.
    Q_PROPERTY(int count READ count NOTIFY readyChanged FINAL)
    /// The scripts the filter offers: [{ key, name, nativeName, label, sample }], Latin first, then by name.
    Q_PROPERTY(QVariantList scripts READ scripts CONSTANT FINAL)
    /// Starred families, and recently used ones (the latest first, at most 24).
    Q_PROPERTY(QStringList favourites READ favourites NOTIFY listsChanged FINAL)
    Q_PROPERTY(QStringList recent READ recent NOTIFY listsChanged FINAL)

public:
    explicit FontCatalogBridge(QObject* parent = nullptr);
    ~FontCatalogBridge() override;
    static FontCatalogBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    bool ready() const { return m_ready; }
    int count() const { return static_cast<int>(m_entries.size()); }
    QVariantList scripts() const;
    QStringList favourites() const { return m_favourites; }
    QStringList recent() const { return m_recent; }

    /// Reads the catalogue now, on the calling thread (tests and a first use that cannot wait); a read on the worker that
    /// is running is dropped.
    void rebuildNow();
    /// Replaces the catalogue (tests: a known set of fonts).
    void setEntries(std::vector<fonts::Entry> entries);

    /// The rows for a search: { rows: [{ family, group, groupLabel, sample, kind, favourite, scriptCount }], total,
    /// ready, note }. `filters`: scripts (keys), bold, italic, minWeights, kinds (keys), source, brandFamilies, sort,
    /// hideUnsupported with `text` (the layer's text, for what must be drawable). `text` typed in the box is `query`.
    Q_INVOKABLE QVariantMap search(const QString& query, const QVariantMap& filters = {}) const;
    /// The characters of `text` the family cannot draw ("" when it draws all; checked on the real font, cached).
    Q_INVOKABLE QString missingGlyphs(const QString& family, const QString& text) const;
    /// The script keys the characters of `text` need (["latin"], ["thai", "latin"]...).
    Q_INVOKABLE QStringList scriptsOfText(const QString& text) const;
    /// The script key of a language code ("th" -> "thai", "zh-Hant" -> "traditionalChinese"), or "".
    Q_INVOKABLE QString scriptOfLanguage(const QString& language) const;
    /// A family was chosen: it leads the recent list.
    Q_INVOKABLE void noteUsed(const QString& family);
    Q_INVOKABLE void setFavourite(const QString& family, bool on);
    Q_INVOKABLE bool isFavourite(const QString& family) const;
    /// The fonts the open project uses: titles and generated media, graphics layers, caption styles and presets, text styles.
    Q_INVOKABLE QStringList projectFamilies() const;
    /// Whether the family is installed (a project can name a font this computer lacks).
    Q_INVOKABLE bool isInstalled(const QString& family) const;

signals:
    void readyChanged();
    void listsChanged();

private:
    void startBuild();
    fonts::Query queryOf(const QString& text, const QVariantMap& filters) const;
    QString headerFor(const fonts::Row& row) const;

    QPointer<ProjectController> m_controller;
    std::vector<fonts::Entry> m_entries;
    bool m_ready = false;
    bool m_namesOnly = true; ///< the entries are bare names until the worker has read the rest
    QStringList m_favourites;
    QStringList m_recent;
    std::jthread m_worker;
    std::atomic<std::uint64_t> m_generation{0};
    mutable std::mutex m_glyphMutex;
    mutable QHash<QString, QString> m_missing; ///< family + '\n' + text -> characters not drawn
};

/// QML registration through the factory (see BrandSingleton).
struct FontCatalogSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::FontCatalogBridge)
    QML_NAMED_ELEMENT(Fonts)
    QML_SINGLETON
public:
    static FontCatalogBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return FontCatalogBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
