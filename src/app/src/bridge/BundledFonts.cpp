#include "BundledFonts.h"

#include <QFont>
#include <QFontDatabase>
#include <QMutex>
#include <QSet>

namespace wabcut::app {

namespace {

constexpr auto kPrefix = ":/qt/qml/Wabcut/resources/fonts/";

// Design language v3: Manrope (UI and display) and JetBrains Mono (numbers), with IBM Plex Sans Thai for Thai in both.
// IBM Plex Sans/Mono and Bai Jamjuree stay bundled: titles, caption styles and templates choose them for the picture.
constexpr const char* kFiles[] = {
    "Manrope-Regular.ttf",         "Manrope-Medium.ttf",         "Manrope-SemiBold.ttf",
    "Manrope-Bold.ttf",            "Manrope-ExtraBold.ttf",
    "JetBrainsMono-Regular.ttf",   "JetBrainsMono-Medium.ttf",   "JetBrainsMono-SemiBold.ttf",
    "JetBrainsMono-Bold.ttf",
    "IBMPlexSans-Regular.ttf",     "IBMPlexSans-Medium.ttf",     "IBMPlexSans-SemiBold.ttf",
    "IBMPlexSansThai-Regular.ttf", "IBMPlexSansThai-Medium.ttf", "IBMPlexSansThai-SemiBold.ttf",
    "IBMPlexSansThai-Bold.ttf",
    "IBMPlexMono-Regular.ttf",     "IBMPlexMono-Medium.ttf",     "IBMPlexMono-SemiBold.ttf",
    "BaiJamjuree-Medium.ttf",      "BaiJamjuree-SemiBold.ttf",
};

constexpr auto kSans = "Manrope";
constexpr auto kThai = "IBM Plex Sans Thai";
constexpr auto kMono = "JetBrains Mono";
constexpr auto kDisplay = "Manrope";
// The v2 faces, still bundled for content (they get the same Thai fallback).
constexpr auto kPlexSans = "IBM Plex Sans";
constexpr auto kPlexMono = "IBM Plex Mono";

QString systemSans() {
#if defined(Q_OS_WIN)
    return QStringLiteral("Segoe UI Variable Text");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("SF Pro Text");
#else
    return QStringLiteral("Inter");
#endif
}

QString systemMono() {
#if defined(Q_OS_WIN)
    return QStringLiteral("Cascadia Mono");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("SF Mono");
#else
    return QStringLiteral("JetBrains Mono");
#endif
}

struct Registry {
    QMutex mutex;
    bool done = false;
    QSet<QString> families;
};

Registry& registry() {
    static Registry r;
    return r;
}

bool has(const char* family) {
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    return r.families.contains(QString::fromLatin1(family));
}

} // namespace

BundledFonts::BundledFonts(QObject* parent) : QObject(parent) {}

BundledFonts* BundledFonts::create(QQmlEngine*, QJSEngine*) {
    registerAll();
    return new BundledFonts;
}

void BundledFonts::registerAll() {
    Registry& r = registry();
    {
        QMutexLocker lock(&r.mutex);
        if (r.done)
            return;
        r.done = true;
        for (const char* file : kFiles) {
            const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(kPrefix) + QString::fromLatin1(file));
            if (id < 0)
                continue;
            for (const QString& family : QFontDatabase::applicationFontFamilies(id))
                r.families.insert(family);
        }
    }
    // Thai in the Latin families: IBM Plex Sans Thai first, before any system font.
    if (has(kThai)) {
        for (const char* family : {kSans, kMono, kPlexSans, kPlexMono})
            QFont::insertSubstitution(QString::fromLatin1(family), QString::fromLatin1(kThai));

        QFont::insertSubstitution(systemSans(), QString::fromLatin1(kThai));
    }
}

QStringList BundledFonts::loadedFamilies() {
    registerAll();
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    QStringList out(r.families.begin(), r.families.end());
    out.sort();
    return out;
}

QString BundledFonts::sans() {
    registerAll();
    return has(kSans) ? QString::fromLatin1(kSans) : systemSans();
}

QString BundledFonts::thai() {
    registerAll();
    return has(kThai) ? QString::fromLatin1(kThai) : systemSans();
}

QString BundledFonts::mono() {
    registerAll();
    return has(kMono) ? QString::fromLatin1(kMono) : systemMono();
}

QString BundledFonts::display() {
    registerAll();
    return has(kDisplay) ? QString::fromLatin1(kDisplay) : sans();
}

bool BundledFonts::bundled() {
    registerAll();
    return has(kSans) && has(kThai) && has(kMono) && has(kDisplay);
}

} // namespace wabcut::app
