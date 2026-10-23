#include "HomeBridge.h"

#include "BrandBridge.h"
#include "BridgeIds.h"
#include "CommandJob.h"
#include "GamingStartBridge.h"
#include "LibraryBridge.h"
#include "MediaBridge.h"
#include "MediaReadinessBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/command/Command.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/Vfr.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QQmlEngine>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace wabcut::app {

using command::Json;

namespace {

const QString kRecentKey = QStringLiteral("home/recentProjects");
/// Preferences.qml's starred projects: a JSON list of the paths as Home.recent shows them.
const QString kStarredKey = QStringLiteral("home/starred");
const QString kDefaultFolderKey = QStringLiteral("home/defaultFolder");
const QString kRecentLimitKey = QStringLiteral("home/recentLimit");
const QString kWebsite = QStringLiteral("https://wabcut.zyphite.com");
/// Characters a project (file) name cannot have, besides control characters.
const QString kInvalidNameCharacters = QStringLiteral("\\/:*?\"<>|");
bool s_showAtLaunch = false;

/// classifyImport probes at most this many files of an unknown kind (media::probe on the GUI thread).
constexpr int kMaxProbes = 16;
/// createFromMedia: the audio streams a plan may name.
constexpr int kMaxAudioStreams = 64;

struct RateChoice {
    const char* label;
    Rate rate;
};
constexpr std::array kRates{
    RateChoice{"23.976", kRate23_976}, RateChoice{"24", kRate24},        RateChoice{"25", kRate25},
    RateChoice{"29.97", kRate29_97},   RateChoice{"30", kRate30},        RateChoice{"50", kRate50},
    RateChoice{"59.94", kRate59_94},   RateChoice{"60", kRate60},        RateChoice{"120", Rate{120, 1}},
    RateChoice{"144", Rate{144, 1}},   RateChoice{"240", Rate{240, 1}},
};

QString rateLabelFor(Rate rate) {
    for (const RateChoice& choice : kRates) {
        if (choice.rate.num == rate.num && choice.rate.den == rate.den) {
            return QString::fromLatin1(choice.label);
        }
    }
    return QLocale::c().toString(rate.toDouble(), 'g', 6);
}

/// 29.97 and 59.94: the rates drop-frame timecode exists for.
bool dropFrameRate(Rate rate) {
    const Rate r = rate.normalized();
    return r.den == 1001 && (r.num == 30000 || r.num == 60000);
}

bool samePath(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), sensitivity) == 0;
}

/// A key that is the same for two spellings of one path (classifyImport's duplicates).
QString pathKey(const QString& path) {
#ifdef Q_OS_WIN
    return QDir::cleanPath(path).toLower();
#else
    return QDir::cleanPath(path);
#endif
}

bool containsPath(const QStringList& paths, const QString& path) {
    return std::ranges::any_of(paths, [&](const QString& p) { return samePath(p, path); });
}

QString localPath(const QUrl& file) {
    return file.isLocalFile() ? file.toLocalFile() : file.toString();
}

/// A web address (an image dragged from a browser: https://...), not a file on this computer. A path QML passed as a
/// string ("C:/Videos/a.mp4": its drive reads as a one-letter scheme) or without a scheme is a file.
bool remoteUrl(const QUrl& url) {
    return !url.isLocalFile() && url.scheme().size() > 1;
}

std::filesystem::path fsPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

/// Why the folder `path`, not there yet, could not be made on Create: the drive or share it is on is not there (an
/// unplugged disk set as the default location), or a file is in the way. "" when it can be made (as far as can be told
/// without making it: Create still reports a folder it then cannot make).
QString folderProblem(const QString& path) {
    QString at = QDir::cleanPath(path);
    for (int up = 0; up < 128 && !at.isEmpty(); ++up) {
        const QFileInfo info(at);
        if (info.exists()) {
            if (info.isDir())
                return {};
            return HomeBridge::tr("Cannot create the folder %1: %2 is a file")
                .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(at));
        }
        const QString parent = QDir::cleanPath(info.path());
        if (parent.isEmpty() || parent == at || parent == QLatin1String(".")) {
            return HomeBridge::tr("Cannot create the folder %1: %2 is not available")
                .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(at));
        }
        at = parent;
    }
    return {};
}

/// A QML value as plain QVariant data (a JS object or array can arrive as a QJSValue).
QVariant plainValue(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant();
    }
    return value;
}

/// The starred projects' paths as Preferences stores them (QSettings "home/starred": a JSON list).
QStringList storedStarred() {
    const QVariant value = QSettings().value(kStarredKey);
    QStringList out;
    if (value.typeId() == QMetaType::QStringList) {
        out = value.toStringList();
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
        for (const QJsonValue& entry : document.array()) {
            if (entry.isString() && !entry.toString().isEmpty()) {
                out << entry.toString();
            }
        }
    }
    out.removeAll(QString());
    return out;
}

/// A stored recent entry ({ path, opened, format, thumbnail }) as QML sees it.
QVariantMap publicEntry(const QVariantMap& stored, const QDateTime& now, const QLocale& locale) {
    const QFileInfo info(stored.value(QStringLiteral("path")).toString());
    const QString openedIso = stored.value(QStringLiteral("opened")).toString();
    const QDateTime opened = QDateTime::fromString(openedIso, Qt::ISODate);
    QString openedText;
    if (opened.isValid()) {
        const qint64 days = opened.date().daysTo(now.date());
        const QString time = locale.toString(opened.time(), QLocale::ShortFormat);
        openedText = days == 0   ? HomeBridge::tr("Today, %1").arg(time)
                     : days == 1 ? HomeBridge::tr("Yesterday, %1").arg(time)
                                 : locale.toString(opened.date(), QLocale::ShortFormat);
    }
    const bool missing = !info.exists();
    return {
        {QStringLiteral("url"), QUrl::fromLocalFile(info.absoluteFilePath())},
        {QStringLiteral("path"), QDir::toNativeSeparators(info.absoluteFilePath())},
        {QStringLiteral("name"), info.completeBaseName()},
        {QStringLiteral("folder"), QDir::toNativeSeparators(info.absolutePath())},
        {QStringLiteral("opened"), opened.isValid() ? openedIso : QString()},
        {QStringLiteral("openedText"), openedText},
        {QStringLiteral("format"), stored.value(QStringLiteral("format")).toString()},
        {QStringLiteral("thumbnail"), missing ? QString() : stored.value(QStringLiteral("thumbnail")).toString()},
        {QStringLiteral("missing"), missing},
    };
}

// ---- What the all-in-one Import takes ----

const QStringList& videoSuffixes() {
    static const QStringList suffixes{
        QStringLiteral("mp4"), QStringLiteral("mov"),  QStringLiteral("mkv"), QStringLiteral("mxf"),
        QStringLiteral("avi"), QStringLiteral("m4v"),  QStringLiteral("webm"), QStringLiteral("flv"),
        QStringLiteral("ts"),  QStringLiteral("m2ts"), QStringLiteral("mts"), QStringLiteral("wmv"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m2v"), QStringLiteral("3gp"),
        QStringLiteral("3g2"), QStringLiteral("ogv"),  QStringLiteral("vob"), QStringLiteral("dv")};
    return suffixes;
}

const QStringList& audioSuffixes() {
    static const QStringList suffixes{
        QStringLiteral("wav"),  QStringLiteral("mp3"), QStringLiteral("aac"), QStringLiteral("m4a"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("aif"),
        QStringLiteral("aiff"), QStringLiteral("aifc"), QStringLiteral("wma"), QStringLiteral("ac3"),
        QStringLiteral("eac3"), QStringLiteral("mka"), QStringLiteral("caf"), QStringLiteral("w64"),
        QStringLiteral("amr")};
    return suffixes;
}

const QStringList& imageSuffixes() {
    static const QStringList suffixes{
        QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("bmp"), QStringLiteral("gif"),  QStringLiteral("webp"),
        QStringLiteral("exr"), QStringLiteral("dpx"),  QStringLiteral("tga"),  QStringLiteral("heic"),
        QStringLiteral("heif"), QStringLiteral("avif")};
    return suffixes;
}

const QStringList& captionSuffixes() {
    static const QStringList suffixes{QStringLiteral("srt"),  QStringLiteral("vtt"),  QStringLiteral("scc"),
                                      QStringLiteral("mcc"),  QStringLiteral("stl"),  QStringLiteral("ttml"),
                                      QStringLiteral("dfxp"), QStringLiteral("itt")};
    return suffixes;
}

const QStringList& timelineSuffixes() {
    static const QStringList suffixes{QStringLiteral("xml"), QStringLiteral("fcpxml"), QStringLiteral("otio"),
                                      QStringLiteral("edl"), QStringLiteral("prproj")};
    return suffixes;
}

/// Files that are certainly not media: refused with a reason, without a probe.
const QStringList& documentSuffixes() {
    static const QStringList suffixes{
        QStringLiteral("txt"), QStringLiteral("md"),   QStringLiteral("pdf"),  QStringLiteral("doc"),
        QStringLiteral("docx"), QStringLiteral("rtf"), QStringLiteral("odt"),  QStringLiteral("xls"),
        QStringLiteral("xlsx"), QStringLiteral("csv"), QStringLiteral("ppt"),  QStringLiteral("pptx"),
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("ini"),  QStringLiteral("cfg"),
        QStringLiteral("log"),  QStringLiteral("exe"), QStringLiteral("dll"),  QStringLiteral("msi"),
        QStringLiteral("lnk"),  QStringLiteral("url"), QStringLiteral("rar"),  QStringLiteral("7z"),
        QStringLiteral("tar")};
    return suffixes;
}

/// Containers the interchange detector explains (a zip archive, gzip, AAF): its guidance says what to do.
const QStringList& archiveSuffixes() {
    static const QStringList suffixes{QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("aaf"),
                                      QStringLiteral("otioz"), QStringLiteral("json")};
    return suffixes;
}

/// Timeline bundles (folders that are one timeline or library): never expanded as folders of media.
bool bundleSuffix(const QString& suffix) {
    return suffix == QLatin1String("fcpxmld") || suffix == QLatin1String("otiod") || suffix == QLatin1String("fcpbundle");
}

QString mediaKindOf(const QString& suffix) {
    if (videoSuffixes().contains(suffix))
        return QStringLiteral("video");
    if (audioSuffixes().contains(suffix))
        return QStringLiteral("audio");
    if (imageSuffixes().contains(suffix))
        return QStringLiteral("image");
    return {};
}

/// A dotLottie archive (.lottie, LottieFiles' zip): Wabcut reads only the Bodymovin JSON inside one.
bool dotLottieFile(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String("lottie"), Qt::CaseInsensitive) == 0;
}

/// A Lottie animation Wabcut imports: Bodymovin JSON.
bool lottieFile(const QString& path) {
    if (QFileInfo(path).suffix().toLower() != QLatin1String("json"))
        return false;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && looksLikeLottie(file.read(64 * 1024));
}

bool layeredFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    return !suffix.isEmpty() && isLayeredFileExtension((QLatin1Char('.') + suffix).toStdString());
}

QString sizeTextOf(const QFileInfo& info) {
    return info.isFile() ? QLocale().formattedDataSize(info.size(), 1, QLocale::DataSizeTraditionalFormat) : QString();
}

QVariantMap fileEntry(const QFileInfo& info) {
    return {{QStringLiteral("url"), QUrl::fromLocalFile(info.absoluteFilePath())},
            {QStringLiteral("path"), QDir::toNativeSeparators(info.absoluteFilePath())},
            {QStringLiteral("name"), info.fileName()}};
}

/// classifyImport's buckets as it fills them.
struct Sorted {
    QVariantList projects;
    QVariantList timelines;
    QVariantList experimental;
    QVariantList unsupported;
    QVariantList media;
    QVariantList captions;
    QVariantList lottie;
    QVariantList layered;
    QSet<QString> seen;
    int probes = 0;
    int examined = 0; ///< files looked at in folders
    int skipped = 0;  ///< files in folders left out (not media)
    int folders = 0;
    bool truncated = false;
};

QString notImportedGuidance(const QString& suffix) {
    return HomeBridge::tr("Wabcut does not import .%1 files. It imports video, audio and image files, captions, Lottie "
                          "animations, Photoshop files, timelines from other editors (Final Cut Pro XML, FCPXML, "
                          "OpenTimelineIO, EDL) and Wabcut projects.")
        .arg(suffix);
}

/// Puts one file in its bucket. `inFolder`: found in a folder given (only media, captions, Lottie and layered files are
/// taken from folders; the rest is counted as skipped).
void sortFile(const QFileInfo& info, bool inFolder, Sorted& out) {
    const QString path = info.absoluteFilePath();
    if (out.seen.contains(pathKey(path)))
        return;
    out.seen.insert(pathKey(path));
    const QString suffix = info.suffix().toLower();
    QVariantMap entry = fileEntry(info);

    // Known media by their extension (no need to read them).
    if (const QString kind = mediaKindOf(suffix); !kind.isEmpty()) {
        entry.insert(QStringLiteral("kind"), kind);
        entry.insert(QStringLiteral("sizeText"), sizeTextOf(info));
        out.media.append(entry);
        return;
    }
    if (inFolder && !captionSuffixes().contains(suffix) && !layeredFile(path) && suffix != QLatin1String("json")) {
        ++out.skipped;
        return;
    }
    // A dotLottie archive: not imported (Graphics Import reads Bodymovin JSON), so say what to do before a project is
    // made for it.
    if (dotLottieFile(path)) {
        entry.insert(QStringLiteral("format"), QString());
        entry.insert(QStringLiteral("formatName"), QString());
        //: dotLottie is a zip format of LottieFiles; Bodymovin is After Effects' Lottie exporter
        entry.insert(QStringLiteral("guidance"), HomeBridge::tr("This is a dotLottie archive: export the animation as "
                                                                "Lottie JSON (Bodymovin) to import it."));
        out.unsupported.append(entry);
        return;
    }
    switch (editor::classifyImportFile(fsPath(path))) {
    case editor::ImportFileKind::Project:
        if (inFolder) {
            ++out.skipped;
            return;
        }
        out.projects.append(entry);
        return;
    case editor::ImportFileKind::Captions: {
        if (const auto format = interchange::captionFormatFromName(suffix.toStdString())) {
            entry.insert(QStringLiteral("format"), QString::fromUtf8(interchange::toString(*format).data()));
            entry.insert(QStringLiteral("formatName"), QString::fromUtf8(interchange::displayName(*format).data()));
        }
        entry.insert(QStringLiteral("sizeText"), sizeTextOf(info));
        out.captions.append(entry);
        return;
    }
    case editor::ImportFileKind::Interchange: {
        if (inFolder) {
            ++out.skipped;
            return;
        }
        const interchange::Detection detection = interchange::ImportRouter::detect(fsPath(path));
        entry.insert(QStringLiteral("format"), QString::fromUtf8(interchange::toString(detection.format).data()));
        entry.insert(QStringLiteral("formatName"), QString::fromUtf8(interchange::displayName(detection.format).data()));
        if (detection.supported) {
            out.timelines.append(entry);
            return;
        }
        entry.insert(QStringLiteral("guidance"), QString::fromStdString(detection.guidance));
        if (detection.format == interchange::Format::PremiereProject)
            out.experimental.append(entry);
        else
            out.unsupported.append(entry);
        return;
    }
    case editor::ImportFileKind::Media:
        break;
    }
    entry.insert(QStringLiteral("sizeText"), sizeTextOf(info));
    if (lottieFile(path)) {
        out.lottie.append(entry);
        return;
    }
    if (layeredFile(path)) {
        out.layered.append(entry);
        return;
    }
    if (inFolder) {
        ++out.skipped;
        return;
    }
    // Not a kind Wabcut knows by its extension: what is it?
    QString guidance;
    if (timelineSuffixes().contains(suffix) || archiveSuffixes().contains(suffix)) {
        if (suffix == QLatin1String("json")) {
            guidance = HomeBridge::tr("This JSON file is not a Lottie animation (Bodymovin JSON), the only JSON files "
                                      "Wabcut imports.");
        } else {
            guidance = QString::fromStdString(interchange::ImportRouter::detect(fsPath(path)).guidance);
        }
    } else if (documentSuffixes().contains(suffix) || suffix.isEmpty()) {
        guidance = notImportedGuidance(suffix.isEmpty() ? info.fileName() : suffix);
    } else if (out.probes < kMaxProbes) {
        ++out.probes;
        const auto probed = media::probe(path.toStdString());
        if (probed && (probed->hasVideo() || probed->hasAudio())) {
            const bool picture = probed->hasVideo() && !probed->isStillImage;
            entry.insert(QStringLiteral("kind"), picture             ? QStringLiteral("video")
                                                 : probed->hasVideo() ? QStringLiteral("image")
                                                                      : QStringLiteral("audio"));
            out.media.append(entry);
            return;
        }
        guidance = probed ? HomeBridge::tr("Wabcut cannot read this file: it has no picture or sound.")
                          : HomeBridge::tr("Wabcut cannot read this file: %1").arg(QString::fromStdString(probed.error().message));
    } else {
        // Too many unknown files to look into here: the import says which ones it could not read.
        entry.insert(QStringLiteral("kind"), QStringLiteral("file"));
        out.media.append(entry);
        return;
    }
    if (guidance.isEmpty())
        guidance = notImportedGuidance(suffix);
    entry.insert(QStringLiteral("format"), QString());
    entry.insert(QStringLiteral("formatName"), QString());
    entry.insert(QStringLiteral("guidance"), guidance);
    out.unsupported.append(entry);
}

/// A folder's files (sorted by name, the subfolders after them), at most kMaxFolderFiles looked at, kMaxFolderDepth deep.
void sortFolder(const QDir& dir, int depth, Sorted& out) {
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& file : files) {
        if (out.examined >= HomeBridge::kMaxFolderFiles) {
            out.truncated = true;
            return;
        }
        ++out.examined;
        sortFile(file, true, out);
    }
    if (depth >= HomeBridge::kMaxFolderDepth)
        return;
    const QFileInfoList folders = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& folder : folders) {
        if (folder.isSymLink() || bundleSuffix(folder.suffix().toLower())) {
            ++out.skipped;
            continue;
        }
        sortFolder(QDir(folder.absoluteFilePath()), depth + 1, out);
        if (out.truncated)
            return;
    }
}

QString baseNameOf(const QVariantList& entries) {
    return entries.isEmpty() ? QString() : QFileInfo(entries.front().toMap().value(QStringLiteral("path")).toString()).completeBaseName();
}

// ---- Media start ----

/// The first video's size as shown and its frame rate, for "Match my media".
struct VideoFormat {
    int width = 0;
    int height = 0;
    Rate rate = kRate30;
};

/// The first file that is a video (any extension: classifyImport probes the ones it does not know, so the New project
/// page's first video can be a .f4v or a .y4m) read the way the page's "Match my media" tile shows it
/// (GamingStartBridge::describe: the size as shown, a variable rate snapped to its nominal one, 60 fps when the rate is
/// unknown), so the project gets the format the tile and the summary said.
std::optional<VideoFormat> firstVideoFormat(const QList<QUrl>& files) {
    int probes = 0;
    for (const QUrl& url : files) {
        const QString path = localPath(url);
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (audioSuffixes().contains(suffix) || imageSuffixes().contains(suffix) || captionSuffixes().contains(suffix) ||
            lottieFile(path) || dotLottieFile(path) || layeredFile(path))
            continue;
        if (++probes > 8)
            break;
        const auto info = media::probe(path.toStdString());
        if (!info || info->video.empty() || info->isStillImage)
            continue;
        const QVariantMap described = GamingStartBridge::describe(*info, path);
        if (!described.value(QStringLiteral("ok")).toBool())
            continue;
        const QVariantMap fps = described.value(QStringLiteral("fps")).toMap();
        VideoFormat format;
        format.width = described.value(QStringLiteral("width")).toInt();
        format.height = described.value(QStringLiteral("height")).toInt();
        const Rate rate{fps.value(QStringLiteral("num")).toInt(), fps.value(QStringLiteral("den")).toInt()};
        if (rate.isExact())
            format.rate = rate.normalized();
        return format;
    }
    return std::nullopt;
}

/// One audio stream of the first video's plan.
struct StreamChoice {
    bool include = true;
    QString name;
};

QString runCommand(ProjectController* controller, const std::string& command, Json params) {
    return commandErrorText(CommandJob::runNow(controller, nullptr, command, std::move(params)));
}

std::string audioLabel(std::size_t position) {
    return "A" + std::to_string(position + 1);
}

/// The first video with its audio plan (the gaming start's steps, at `at`): one audio track per included stream
/// (sequence.addTrack while the sequence has fewer; new ones named), the picture on V1 and the included streams on A1,
/// A2, ... in stream order (edit.insertFromSource with explicit patches: a stream left out is not patched), then the
/// names of the tracks the sequence already had. "" or what went wrong.
QString placeWithPlan(ProjectController* controller, const MediaItem& media, const std::vector<StreamChoice>& plan, Ticks at) {
    const std::size_t audioCount = media.info.audio.size();
    const auto included = [&](std::size_t stream) { return stream >= plan.size() || plan[stream].include; };
    const auto trackName = [&](std::size_t stream) {
        return stream < plan.size() && !plan[stream].name.isEmpty() ? plan[stream].name
                                                                    : GamingStartBridge::suggestedTrackName(static_cast<int>(stream));
    };
    std::vector<std::size_t> streams;
    for (std::size_t i = 0; i < audioCount; ++i) {
        if (included(i))
            streams.push_back(i);
    }
    const SequencePtr sequence = controller->currentSequence();
    if (!sequence)
        return HomeBridge::tr("the project has no sequence");
    const std::size_t existing = sequence->audioTracks.size();
    for (std::size_t have = existing; have < streams.size(); ++have) {
        const QString track = trackName(streams[have]);
        if (const QString error = runCommand(controller, "sequence.addTrack", Json{{"kind", "audio"}, {"name", track.toStdString()}});
            !error.isEmpty())
            return HomeBridge::tr("the audio track \"%1\" could not be added: %2").arg(track, error);
    }
    Json patches = Json::array();
    if (media.info.hasVideo())
        patches.push_back(Json{{"source", "V1"}, {"track", "V1"}});
    std::size_t next = 0;
    for (std::size_t i = 0; i < audioCount; ++i) {
        if (included(i))
            patches.push_back(Json{{"source", audioLabel(i)}, {"track", audioLabel(next++)}});
        else
            patches.push_back(Json{{"source", audioLabel(i)}, {"track", nullptr}});
    }
    if (const QString error = runCommand(controller, "edit.insertFromSource",
                                         Json{{"media", media.id.value},
                                              {"at", at.value},
                                              {"patches", std::move(patches)},
                                              {"sequenceMarks", false}});
        !error.isEmpty())
        return error;
    for (std::size_t j = 0; j < std::min(existing, streams.size()); ++j) {
        const QString track = trackName(streams[j]);
        if (const QString error = runCommand(controller, "track.setState", Json{{"track", audioLabel(j)}, {"name", track.toStdString()}});
            !error.isEmpty())
            return HomeBridge::tr("track %1 could not be named \"%2\": %3").arg(QString::fromStdString(audioLabel(j)), track, error);
    }
    return {};
}

/// Where the next clip of a media start goes: the end of the last clip of the sequence, up to the next frame (a clip
/// whose audio runs a little longer than its picture does not get its tail overwritten).
Ticks nextClipStart(const Sequence& sequence) {
    Ticks end{};
    sequence.forEachTrack([&](const Track& track) {
        for (const Clip& clip : track.clips)
            end = std::max(end, clip.end());
    });
    if (!sequence.frameRate.isExact())
        return end;
    const Ticks frame = snapToFrame(end, sequence.frameRate);
    return frame == end ? end : framesToTicks(ticksToFrames(end, sequence.frameRate) + 1, sequence.frameRate);
}

bool sameFile(const QString& a, const QString& b) {
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return !ca.isEmpty() && ca.compare(cb, sensitivity) == 0;
}

} // namespace

HomeBridge::HomeBridge(QObject* parent) : QObject(parent) {
    load();
}

HomeBridge* HomeBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* home = new HomeBridge;
    if (qmlEngine) {
        home->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        home->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        home->setEngine(qmlEngine); // MediaReadiness: resolved when proxies are asked for
    }
    return home;
}

void HomeBridge::setShowAtLaunch(bool show) {
    s_showAtLaunch = show;
}

bool HomeBridge::showAtLaunch() const {
    return s_showAtLaunch;
}

void HomeBridge::setLifecycle(ProjectLifecycle* lifecycle) {
    if (m_lifecycle == lifecycle) {
        return;
    }
    if (m_lifecycle) {
        disconnect(m_lifecycle, nullptr, this, nullptr);
    }
    m_lifecycle = lifecycle;
    if (m_lifecycle) {
        connect(m_lifecycle, &ProjectLifecycle::documentChanged, this, &HomeBridge::onDocumentChanged);
    }
    onDocumentChanged();
}

void HomeBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

void HomeBridge::setEngine(QQmlEngine* engine) {
    m_engine = engine;
}

void HomeBridge::setProxyRequester(ProxyRequester requester) {
    m_proxyRequester = std::move(requester);
}

// ---- New project ----------------------------------------------------------------------------------------

QVariantList HomeBridge::presets() const {
    struct Preset {
        const char* id;
        QString name;
        int width;
        int height;
        Rate rate;
        const char* shape;
        const char* group;
        bool rateChoice;
        bool custom;
    };
    // The first nine keep their ids (homePreset_<id>); the UX rework adds 1440p60, UHD 60 and vertical 60 after them.
    const std::array presets{
        Preset{"1080p23976", tr("1080p"), 1920, 1080, kRate23_976, "landscape", "landscape", false, false},
        Preset{"1080p25", tr("1080p"), 1920, 1080, kRate25, "landscape", "landscape", false, false},
        Preset{"1080p2997", tr("1080p"), 1920, 1080, kRate29_97, "landscape", "landscape", false, false},
        Preset{"1080p30", tr("1080p"), 1920, 1080, kRate30, "landscape", "landscape", false, false},
        Preset{"1080p60", tr("1080p"), 1920, 1080, kRate60, "landscape", "landscape", false, false},
        Preset{"uhd", tr("4K UHD"), 3840, 2160, kRate23_976, "landscape", "landscape", true, false},
        Preset{"vertical", tr("Vertical 9:16"), 1080, 1920, kRate30, "portrait", "vertical", true, false},
        Preset{"square", tr("Square 1:1"), 1080, 1080, kRate30, "square", "vertical", true, false},
        Preset{"custom", tr("Custom"), 1920, 1080, kRate30, "landscape", "custom", true, true},
        Preset{"1440p60", tr("1440p"), 2560, 1440, kRate60, "landscape", "landscape", false, false},
        Preset{"uhd60", tr("4K UHD"), 3840, 2160, kRate60, "landscape", "landscape", false, false},
        Preset{"vertical60", tr("Vertical 9:16"), 1080, 1920, kRate60, "portrait", "vertical", false, false},
    };
    QVariantList out;
    for (const Preset& p : presets) {
        out.append(QVariantMap{
            {QStringLiteral("id"), QString::fromLatin1(p.id)},
            {QStringLiteral("name"), p.name},
            {QStringLiteral("group"), QString::fromLatin1(p.group)},
            {QStringLiteral("rateLabel"), rateLabelFor(p.rate)},
            {QStringLiteral("sizeText"), tr("%1 × %2").arg(p.width).arg(p.height)},
            {QStringLiteral("width"), p.width},
            {QStringLiteral("height"), p.height},
            {QStringLiteral("rateNum"), p.rate.num},
            {QStringLiteral("rateDen"), p.rate.den},
            {QStringLiteral("shape"), QString::fromLatin1(p.shape)},
            {QStringLiteral("rateChoice"), p.rateChoice},
            {QStringLiteral("custom"), p.custom},
        });
    }
    return out;
}

QVariantList HomeBridge::rates() const {
    QVariantList out;
    for (const RateChoice& choice : kRates) {
        out.append(QVariantMap{{QStringLiteral("label"), QString::fromLatin1(choice.label)},
                               {QStringLiteral("rateNum"), choice.rate.num},
                               {QStringLiteral("rateDen"), choice.rate.den},
                               {QStringLiteral("canDropFrame"), dropFrameRate(choice.rate)}});
    }
    return out;
}

QVariantMap HomeBridge::advancedDefaults() const {
    const edit::SequenceSettings defaults;
    return {
        {QStringLiteral("audioRate"), defaults.audioRate.num / std::max(1, defaults.audioRate.den)},
        {QStringLiteral("dropFrame"), defaults.dropFrame},
        {QStringLiteral("videoTracks"), defaults.videoTracks},
        {QStringLiteral("audioTracks"), defaults.audioTracks},
        {QStringLiteral("trackFormat"), QStringLiteral("stereo")},
        {QStringLiteral("masterFormat"), QStringLiteral("stereo")},
        {QStringLiteral("pixelAspect"), QVariantMap{{QStringLiteral("num"), 1}, {QStringLiteral("den"), 1}}},
        {QStringLiteral("fields"), QStringLiteral("progressive")},
        {QStringLiteral("startTimecode"), QStringLiteral("00:00:00:00")},
        {QStringLiteral("previewFormat"), QString()},
        {QStringLiteral("colorManagement"),
         QVariantMap{{QStringLiteral("mode"), QString::fromLatin1(toString(ColorManagement{}.mode))}}},
    };
}

QVariantMap HomeBridge::advancedChoices() const {
    const auto choice = [](const QString& id, const QString& label) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), label}};
    };
    const auto aspect = [](const QString& id, const QString& label, int num, int den) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), label}, {QStringLiteral("num"), num},
                           {QStringLiteral("den"), den}};
    };
    const auto audioRate = [](int hz, const QString& label) {
        return QVariantMap{{QStringLiteral("value"), hz}, {QStringLiteral("label"), label}};
    };
    return {
        {QStringLiteral("audioRates"),
         QVariantList{audioRate(44100, tr("44.1 kHz")), audioRate(48000, tr("48 kHz")), audioRate(96000, tr("96 kHz"))}},
        {QStringLiteral("pixelAspects"),
         QVariantList{aspect(QStringLiteral("square"), tr("Square (1.0)"), 1, 1),
                      aspect(QStringLiteral("d1"), tr("D1/DV NTSC (0.9091)"), 10, 11),
                      aspect(QStringLiteral("anamorphic"), tr("Anamorphic (2.0)"), 2, 1)}},
        {QStringLiteral("fields"), QVariantList{choice(QStringLiteral("progressive"), tr("Progressive")),
                                                choice(QStringLiteral("upperFirst"), tr("Upper field first")),
                                                choice(QStringLiteral("lowerFirst"), tr("Lower field first"))}},
        {QStringLiteral("masterFormats"),
         QVariantList{choice(QStringLiteral("stereo"), tr("Stereo")), choice(QStringLiteral("5.1"), tr("5.1"))}},
        {QStringLiteral("trackFormats"),
         QVariantList{choice(QStringLiteral("stereo"), tr("Stereo")), choice(QStringLiteral("mono"), tr("Mono"))}},
        {QStringLiteral("previewFormats"),
         QVariantList{choice(QString(), tr("Auto")), choice(QStringLiteral("prores"), tr("ProRes")),
                      choice(QStringLiteral("dnxhr"), tr("DNxHR")), choice(QStringLiteral("h264"), tr("H.264"))}},
    };
}

QUrl HomeBridge::defaultFolder() const {
    if (const QUrl chosen = defaultFolderSetting(); !chosen.isEmpty()) {
        return chosen;
    }
    for (const QVariant& entry : m_recent) {
        const QVariantMap map = entry.toMap();
        if (!map.value(QStringLiteral("missing")).toBool()) {
            return QUrl::fromLocalFile(QFileInfo(map.value(QStringLiteral("path")).toString()).absolutePath());
        }
    }
    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (movies.isEmpty()) {
        movies = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    return QUrl::fromLocalFile(movies + QStringLiteral("/Wabcut"));
}

QUrl HomeBridge::defaultFolderSetting() const {
    const QString path = QSettings().value(kDefaultFolderKey).toString();
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

void HomeBridge::setDefaultFolderSetting(const QUrl& folder) {
    const QString path = folder.isEmpty() ? QString() : QDir::cleanPath(localPath(folder));
    if (path == QSettings().value(kDefaultFolderKey).toString()) {
        return;
    }
    QSettings settings;
    if (path.isEmpty()) {
        settings.remove(kDefaultFolderKey);
    } else {
        settings.setValue(kDefaultFolderKey, path);
    }
    emit defaultFolderChanged();
}

int HomeBridge::recentLimit() const {
    return std::clamp(QSettings().value(kRecentLimitKey, kMaxRecent).toInt(), kMinRecent, kMaxRecentLimit);
}

void HomeBridge::setRecentLimit(int limit) {
    limit = std::clamp(limit, kMinRecent, kMaxRecentLimit);
    if (limit == recentLimit()) {
        return;
    }
    QSettings().setValue(kRecentLimitKey, limit);
    emit recentLimitChanged();
    // UX rework fix: only what is listed changes - a lower limit hides the older projects, a higher one lists them
    // again (the history itself keeps up to kMaxRecentLimit: rememberProject).
    refreshRecent();
}

QUrl HomeBridge::projectUrl(const QString& name, const QUrl& folder) const {
    const QString base = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    return QUrl::fromLocalFile(QDir(base).filePath(name.trimmed() + QStringLiteral(".wabcut")));
}

QString HomeBridge::projectPath(const QString& name, const QUrl& folder) const {
    return QDir::toNativeSeparators(projectUrl(name, folder).toLocalFile());
}

QString HomeBridge::validateProject(const QString& name, const QUrl& folder) const {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return tr("Name the project");
    }
    for (const QChar c : trimmed) {
        if (kInvalidNameCharacters.contains(c) || c.unicode() < 0x20) {
            return tr("A project name cannot contain %1").arg(QString(c));
        }
    }
    if (trimmed.endsWith(QLatin1Char('.'))) {
        return tr("A project name cannot end with a period");
    }
    if (folder.isEmpty() || (!folder.isLocalFile() && !QFileInfo(folder.toString()).isAbsolute())) {
        return tr("Choose where to save the project");
    }
    const QFileInfo dir(folder.isLocalFile() ? folder.toLocalFile() : folder.toString());
    if (dir.exists() && !dir.isDir()) {
        return tr("%1 is a file, not a folder").arg(QDir::toNativeSeparators(dir.absoluteFilePath()));
    }
    if (!dir.exists()) {
        if (const QString why = folderProblem(dir.absoluteFilePath()); !why.isEmpty()) {
            return why;
        }
    }
    if (QFileInfo::exists(projectUrl(trimmed, folder).toLocalFile())) {
        return tr("“%1” already exists in this folder").arg(trimmed);
    }
    return {};
}

namespace {

/// createProject's settings as ProjectFile.newProjectWith takes them: `sequenceName` becomes the sequence's `name` (a
/// preset's `name` is not one), `match` is createFromMedia's.
QVariantMap newProjectSettings(const QVariantMap& settings) {
    QVariantMap sequence = settings;
    sequence.remove(QStringLiteral("name"));
    sequence.remove(QStringLiteral("match"));
    sequence.remove(QStringLiteral("sequenceName"));
    if (const QString name = settings.value(QStringLiteral("sequenceName")).toString().trimmed(); !name.isEmpty()) {
        sequence.insert(QStringLiteral("name"), name);
    }
    return sequence;
}

/// What making a project did: `error` ("" when all went well) and whether the new project replaced the one shown
/// (`created`: then only its first save can have failed, and it is open, untitled).
struct Made {
    QString error;
    bool created = false;
};

/// createProject's steps. Everything that can be checked first (the name, the folder, the settings, making the folder)
/// is, so a refusal changes nothing.
Made makeProject(const HomeBridge& home, ProjectLifecycle* lifecycle, const QString& name, const QUrl& folder,
                 const QVariantMap& settings) {
    if (!lifecycle) {
        return {HomeBridge::tr("Projects are not available")};
    }
    if (const QString why = home.validateProject(name, folder); !why.isEmpty()) {
        return {why};
    }
    const QVariantMap sequence = newProjectSettings(settings); // the sequence keeps its default name ("Sequence 01")
    if (const QString why = ProjectLifecycle::checkNewProjectSettings(sequence); !why.isEmpty()) {
        return {why}; // before the folder is made: nothing is created
    }
    const QString dir = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (!QDir().mkpath(dir)) {
        return {HomeBridge::tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(dir))};
    }
    if (!lifecycle->newProjectWith(sequence)) {
        return {lifecycle->lastError()};
    }
    if (!lifecycle->saveAs(home.projectUrl(name, folder))) {
        return {lifecycle->lastError(), true};
    }
    return {QString(), true};
}

} // namespace

QString HomeBridge::validateSettings(const QVariantMap& settings) const {
    return ProjectLifecycle::checkNewProjectSettings(newProjectSettings(settings));
}

QString HomeBridge::uniqueProjectName(const QString& base, const QUrl& folder) const {
    QString name = base;
    for (QChar& c : name) {
        if (kInvalidNameCharacters.contains(c) || c.unicode() < 0x20) {
            c = QLatin1Char(' ');
        }
    }
    name = name.simplified();
    while (name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
        name = name.trimmed();
    }
    if (name.isEmpty()) {
        name = tr("Untitled Project");
    }
    QString candidate = name;
    for (int n = 2; n < 10000 && QFileInfo::exists(projectUrl(candidate, folder).toLocalFile()); ++n) {
        candidate = QStringLiteral("%1 %2").arg(name).arg(n);
    }
    return candidate;
}

QString HomeBridge::rateLabel(int rateNum, int rateDen) const {
    const Rate rate{rateNum, rateDen};
    return rate.isValid() ? rateLabelFor(rate.normalized()) : QString();
}

QString HomeBridge::settingsSummary(const QVariantMap& settings) const {
    edit::SequenceSettings sequence;
    if (const QString id = settings.value(QStringLiteral("preset")).toString(); !id.isEmpty()) {
        if (const SequencePreset* preset = findSequencePreset(id.toStdString())) {
            sequence.width = preset->width;
            sequence.height = preset->height;
            sequence.frameRate = preset->frameRate;
        }
    }
    const int width = settings.value(QStringLiteral("width"), sequence.width).toInt();
    const int height = settings.value(QStringLiteral("height"), sequence.height).toInt();
    const QString rate = rateLabel(settings.value(QStringLiteral("rateNum"), sequence.frameRate.num).toInt(),
                                   settings.value(QStringLiteral("rateDen"), sequence.frameRate.den).toInt());
    const int audioRate = settings.value(QStringLiteral("audioRate"), sequence.audioRate.num).toInt();
    const QString master = settings.value(QStringLiteral("masterFormat")).toString() == QLatin1String("5.1")
                               ? tr("5.1")
                               : tr("stereo");
    const int videoTracks = settings.value(QStringLiteral("videoTracks"), sequence.videoTracks).toInt();
    const int audioTracks = settings.value(QStringLiteral("audioTracks"), sequence.audioTracks).toInt();
    //: New project summary: "1920 × 1080 · 29.97 fps · 48 kHz stereo · 3 video / 3 audio tracks"
    return tr("%1 × %2 · %3 fps · %4 kHz %5 · %6 video / %7 audio tracks")
        .arg(width)
        .arg(height)
        .arg(rate, QString::number(audioRate / 1000.0, 'g', 4), master)
        .arg(videoTracks)
        .arg(audioTracks);
}

QString HomeBridge::createProject(const QString& name, const QUrl& folder, const QVariantMap& settings) {
    return makeProject(*this, m_lifecycle, name, folder, settings).error;
}

QVariantMap HomeBridge::createFromMedia(const QString& name, const QUrl& folder, const QVariantMap& settings,
                                        const QList<QUrl>& files, const QVariantMap& options) {
    return createFromMediaWith(name, folder, settings, files, options, {});
}

int HomeBridge::requestProxies(const QStringList& mediaKeys, const ProxyRequester& requester) {
    if (requester)
        return requester(mediaKeys);
    if (m_proxyRequester)
        return m_proxyRequester(mediaKeys);
    auto* readiness = m_engine ? m_engine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness") : nullptr;
    return readiness ? readiness->generateProxies(mediaKeys) : 0;
}

QVariantMap HomeBridge::createFromMediaWith(const QString& name, const QUrl& folder, const QVariantMap& settings,
                                            const QList<QUrl>& files, const QVariantMap& options,
                                            const ProxyRequester& requester) {
    QVariantMap result{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), QString()},
                       {QStringLiteral("warning"), QString()},
                       {QStringLiteral("project"), QUrl()},
                       {QStringLiteral("added"), 0},
                       {QStringLiteral("captionTracks"), 0},
                       {QStringLiteral("failed"), QVariantList{}},
                       {QStringLiteral("placed"), 0},
                       {QStringLiteral("deferred"), QVariantList{}}};
    const auto refuse = [&](const QString& error) {
        result.insert(QStringLiteral("error"), error);
        return result;
    };
    if (!m_lifecycle || !m_controller) {
        return refuse(tr("Projects are not available"));
    }

    // The files: media and captions are imported here; Lottie animations and layered Photoshop files go to Main's
    // importUrls once the project exists (their own import dialogs and reports).
    QList<QUrl> importable;
    QVariantList deferred;
    for (const QUrl& url : files) {
        const QString path = localPath(url);
        // A web address or a dotLottie archive is nothing this can import (classifyImport says why).
        if (path.isEmpty() || remoteUrl(url) || dotLottieFile(path))
            continue;
        const QUrl absolute = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
        if (lottieFile(path) || layeredFile(path)) {
            deferred.append(absolute);
            continue;
        }
        if (!mediaKindOf(QFileInfo(path).suffix().toLower()).isEmpty()) {
            importable.append(absolute);
            continue;
        }
        const editor::ImportFileKind kind = editor::classifyImportFile(fsPath(path));
        if (kind == editor::ImportFileKind::Media || kind == editor::ImportFileKind::Captions)
            importable.append(absolute);
    }
    if (importable.isEmpty() && deferred.isEmpty()) {
        return refuse(tr("Choose the media to start with"));
    }

    // The first video's audio plan.
    std::vector<StreamChoice> plan;
    const QVariantList tracks = plainValue(options.value(QStringLiteral("tracks"))).toList();
    for (const QVariant& value : tracks) {
        const QVariantMap track = plainValue(value).toMap();
        bool ok = false;
        const int index = track.value(QStringLiteral("index")).toInt(&ok);
        if (!ok || index < 0 || index >= kMaxAudioStreams) {
            return refuse(tr("There is no audio stream %1").arg(track.value(QStringLiteral("index")).toString()));
        }
        if (plan.size() <= static_cast<std::size_t>(index))
            plan.resize(static_cast<std::size_t>(index) + 1);
        if (track.contains(QStringLiteral("include")))
            plan[static_cast<std::size_t>(index)].include = track.value(QStringLiteral("include")).toBool();
        if (const QString trackName = track.value(QStringLiteral("name")).toString().trimmed(); !trackName.isEmpty())
            plan[static_cast<std::size_t>(index)].name = trackName;
    }
    const bool planGiven = !tracks.isEmpty();

    // The sequence: as given, or matched to the first video ("Match my media").
    QVariantMap sequence = settings;
    const bool match = sequence.value(QStringLiteral("match")).toBool() ||
                       (!sequence.contains(QStringLiteral("width")) && !sequence.contains(QStringLiteral("height")) &&
                        !sequence.contains(QStringLiteral("preset")));
    sequence.remove(QStringLiteral("match"));
    if (match) {
        if (const auto format = firstVideoFormat(importable)) {
            if (format->width >= 16 && format->height >= 16 && format->width <= 16384 && format->height <= 16384) {
                sequence.insert(QStringLiteral("width"), format->width);
                sequence.insert(QStringLiteral("height"), format->height);
            }
            sequence.insert(QStringLiteral("rateNum"), format->rate.num);
            sequence.insert(QStringLiteral("rateDen"), format->rate.den);
        }
    }

    // 1. The project, saved and opened. A refusal before it exists changes nothing. Once it exists (it replaced the
    // project shown), what goes wrong is a warning and the rest goes on - even when its first save failed (a folder
    // that cannot be written): the media go into it, it stays open (untitled) and the warning says to Save As.
    const Made made = makeProject(*this, m_lifecycle, name, folder, sequence);
    if (!made.created) {
        return refuse(made.error);
    }
    const bool saved = made.error.isEmpty();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("project"), saved ? projectUrl(name, folder) : QUrl());
    result.insert(QStringLiteral("deferred"), deferred);
    QStringList warnings;
    if (!saved) {
        warnings << tr("%1. The project is open but not saved: save it with File > Save As.").arg(made.error);
    }

    // 2. The files in the project, and their media items in the order given.
    std::vector<MediaPtr> items;
    if (!importable.isEmpty()) {
        const QVariantMap imported = m_lifecycle->importFiles(importable);
        result.insert(QStringLiteral("added"), imported.value(QStringLiteral("added")).toInt());
        result.insert(QStringLiteral("captionTracks"), imported.value(QStringLiteral("captionTracks")).toInt());
        const QVariantList failed = imported.value(QStringLiteral("failed")).toList();
        result.insert(QStringLiteral("failed"), failed);
        for (const QVariant& value : failed) {
            const QVariantMap failure = value.toMap();
            warnings << tr("%1 could not be imported: %2")
                            .arg(failure.value(QStringLiteral("name")).toString(),
                                 failure.value(QStringLiteral("error")).toString());
        }
        if (const ProjectSnapshot snapshot = m_controller->snapshot()) {
            for (const QUrl& url : importable) {
                const QString path = url.toLocalFile();
                for (const MediaPtr& item : snapshot->media) {
                    if (item && sameFile(QString::fromStdString(item->locator.path), path)) {
                        if (std::ranges::find(items, item) == items.end())
                            items.push_back(item);
                        break;
                    }
                }
            }
        }
    }

    // 3. The clips in the sequence, one after another from 00:00.
    int placed = 0;
    if (options.value(QStringLiteral("addToSequence"), true).toBool()) {
        Ticks cursor{};
        bool firstVideo = true;
        for (const MediaPtr& media : items) {
            const SequencePtr current = m_controller->currentSequence();
            if (!current) {
                warnings << tr("The project has no sequence to edit the clips into.");
                break;
            }
            const bool video = media->info.hasVideo() && !media->info.isStillImage;
            const bool planned = video && firstVideo && (planGiven || media->info.audio.size() > 1);
            if (video)
                firstVideo = false;
            QString error;
            if (planned) {
                error = placeWithPlan(m_controller, *media, plan, cursor);
            } else {
                editor::MediaDrop drop;
                drop.media = media->id;
                drop.sequence = current->id;
                drop.at = cursor;
                drop.mode = editor::DropMode::Overwrite; // dropped outside the tracks: the targeted ones (V1, A1)
                if (auto edited = editor::dropMedia(m_controller->session(), drop); !edited)
                    error = QString::fromStdString(edited.error().message);
            }
            if (!error.isEmpty()) {
                warnings << tr("%1 could not be edited into the sequence: %2").arg(QString::fromStdString(media->name), error);
                continue;
            }
            ++placed;
            if (const SequencePtr after = m_controller->currentSequence())
                cursor = std::max(cursor, nextClipStart(*after));
        }
        if (placed > 0) {
            m_controller->setPlayhead({});
        }
    }
    result.insert(QStringLiteral("placed"), placed);

    // 4. Proxies for the first video when it calls for them (4K, above 60 fps, 20 minutes or longer), unless it has one.
    const QVariant proxyOption = options.value(QStringLiteral("proxies"), QStringLiteral("auto"));
    const QString proxies = proxyOption.typeId() == QMetaType::Bool
                                ? (proxyOption.toBool() ? QStringLiteral("auto") : QStringLiteral("off"))
                                : proxyOption.toString();
    if (proxies != QLatin1String("off")) {
        const auto first = std::ranges::find_if(items, [](const MediaPtr& m) { return m->info.hasVideo() && !m->info.isStillImage; });
        if (first != items.end()) {
            const MediaItem& media = **first;
            const bool wanted = proxies == QLatin1String("on") ||
                                GamingStartBridge::describe(media.info, QString::fromStdString(media.locator.path))
                                    .value(QStringLiteral("proxies"))
                                    .toBool();
            if (wanted && !media.locator.proxy && requestProxies({mediaKey(media.id)}, requester) <= 0) {
                warnings << tr("The project is ready, but its proxies could not be started: use Proxy > Create proxy on "
                               "the clip in the Library.");
            }
        }
    }

    // 5. Saved as it is now (unless it has no file: said above).
    if (saved && !m_lifecycle->save()) {
        warnings << tr("The project could not be saved: %1").arg(m_lifecycle->lastError());
    }
    result.insert(QStringLiteral("warning"), warnings.join(QLatin1Char('\n')));
    return result;
}

// ---- Import -----------------------------------------------------------------------------------------------------

QVariantMap HomeBridge::classifyImport(const QList<QUrl>& files) const {
    Sorted sorted;
    for (const QUrl& url : files) {
        // A web address (an image dragged from a browser carries https://...): not a file Wabcut can read.
        if (remoteUrl(url)) {
            const QString shown = url.toDisplayString();
            if (sorted.seen.contains(shown))
                continue;
            sorted.seen.insert(shown);
            QString name = QFileInfo(url.path()).fileName();
            if (name.isEmpty())
                name = url.host().isEmpty() ? shown : url.host();
            sorted.unsupported.append(QVariantMap{
                {QStringLiteral("url"), url},
                {QStringLiteral("path"), shown},
                {QStringLiteral("name"), name},
                {QStringLiteral("format"), QString()},
                {QStringLiteral("formatName"), QString()},
                {QStringLiteral("guidance"), tr("This is a web address, not a file on this computer: save the file "
                                                "to disk first, then import it.")},
            });
            continue;
        }
        const QString path = localPath(url);
        if (path.isEmpty())
            continue;
        const QFileInfo info(path);
        if (info.isDir() && !bundleSuffix(info.suffix().toLower())) {
            ++sorted.folders;
            if (!sorted.truncated)
                sortFolder(QDir(info.absoluteFilePath()), 1, sorted);
            continue;
        }
        sortFile(info, false, sorted);
    }

    const int total = static_cast<int>(sorted.projects.size() + sorted.timelines.size() + sorted.experimental.size() +
                                       sorted.unsupported.size() + sorted.media.size() + sorted.captions.size() +
                                       sorted.lottie.size() + sorted.layered.size());
    const bool hasMedia =
        !sorted.media.isEmpty() || !sorted.captions.isEmpty() || !sorted.lottie.isEmpty() || !sorted.layered.isEmpty();
    QString route;
    QString message;
    QString suggestedName;
    QStringList notes;
    const auto guidanceLine = [](const QVariant& entry) {
        const QVariantMap map = entry.toMap();
        return tr("%1: %2").arg(map.value(QStringLiteral("name")).toString(), map.value(QStringLiteral("guidance")).toString());
    };
    if (!sorted.projects.isEmpty()) {
        // 1. A project opens; nothing else is imported.
        route = QStringLiteral("project");
        suggestedName = baseNameOf(sorted.projects);
        if (total > 1) {
            notes << tr("Only %1 opens: the other %n file(s) were not imported.", "", total - 1)
                         .arg(sorted.projects.front().toMap().value(QStringLiteral("name")).toString());
        }
    } else if (!sorted.timelines.isEmpty()) {
        // 2. Timelines (with any media) become a new project.
        route = QStringLiteral("timelines");
        suggestedName = baseNameOf(sorted.timelines);
        for (const QVariant& entry : sorted.experimental) {
            notes << tr("%1 was left out: a Premiere Pro project is read on its own, with the experimental reader.")
                         .arg(entry.toMap().value(QStringLiteral("name")).toString());
        }
    } else if (!sorted.experimental.isEmpty()) {
        // 3. A Premiere project: only after the user chose the experimental reader.
        route = QStringLiteral("experimental");
        suggestedName = baseNameOf(sorted.experimental);
        if (sorted.experimental.size() > 1) {
            notes << tr("Only %1 is read: the other %n Premiere Pro project(s) were left out.", "",
                        static_cast<int>(sorted.experimental.size()) - 1)
                         .arg(sorted.experimental.front().toMap().value(QStringLiteral("name")).toString());
        }
    } else if (hasMedia) {
        // 5. Media: the New project page, "Start with media".
        route = QStringLiteral("media");
        for (const QVariantList* bucket : {&sorted.media, &sorted.captions, &sorted.lottie, &sorted.layered}) {
            if (!bucket->isEmpty()) {
                suggestedName = baseNameOf(*bucket);
                break;
            }
        }
    } else if (!sorted.unsupported.isEmpty()) {
        // 4. Only files Wabcut cannot read: what to do instead.
        route = QStringLiteral("unsupported");
        message = guidanceLine(sorted.unsupported.front());
    } else {
        route = QStringLiteral("none");
        message = sorted.folders > 0 ? tr("No media, captions or graphics were found in the folder.")
                                     : tr("There is nothing to import.");
    }
    if (route != QLatin1String("project")) {
        const qsizetype first = route == QLatin1String("unsupported") ? 1 : 0;
        for (qsizetype i = first; i < sorted.unsupported.size(); ++i) {
            notes << guidanceLine(sorted.unsupported[i]);
        }
    }
    if (sorted.skipped > 0) {
        notes << tr("%n file(s) in the folders were left out: only media, captions and graphics are taken from folders.",
                    "", sorted.skipped);
    }
    if (sorted.truncated) {
        notes << tr("Only the first %n files of the folders were looked at.", "", kMaxFolderFiles);
    }

    QUrl firstVideo;
    for (const QVariant& entry : sorted.media) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("kind")).toString() == QLatin1String("video")) {
            firstVideo = map.value(QStringLiteral("url")).toUrl();
            break;
        }
    }
    return {
        {QStringLiteral("route"), route},
        {QStringLiteral("projects"), sorted.projects},
        {QStringLiteral("timelines"), sorted.timelines},
        {QStringLiteral("experimental"), sorted.experimental},
        {QStringLiteral("unsupported"), sorted.unsupported},
        {QStringLiteral("media"), sorted.media},
        {QStringLiteral("captions"), sorted.captions},
        {QStringLiteral("lottie"), sorted.lottie},
        {QStringLiteral("layered"), sorted.layered},
        {QStringLiteral("firstVideo"), firstVideo},
        {QStringLiteral("hasVideo"), !firstVideo.isEmpty()},
        {QStringLiteral("suggestedName"), suggestedName},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("message"), message},
        {QStringLiteral("folders"), sorted.folders},
        {QStringLiteral("skipped"), sorted.skipped},
        {QStringLiteral("truncated"), sorted.truncated},
    };
}

QStringList HomeBridge::importNameFilters() const {
    QStringList media;
    for (const QStringList* suffixes : {&videoSuffixes(), &audioSuffixes(), &imageSuffixes()}) {
        for (const QString& suffix : *suffixes)
            media << QStringLiteral("*.") + suffix;
    }
    media << QStringLiteral("*.psd") << QStringLiteral("*.psb") << QStringLiteral("*.json"); // not .lottie (dotLottie)
    QStringList timelines;
    for (const QString& suffix : timelineSuffixes())
        timelines << QStringLiteral("*.") + suffix;
    QStringList captions;
    for (const QString& suffix : captionSuffixes())
        captions << QStringLiteral("*.") + suffix;
    const QString space = QStringLiteral(" ");
    const QStringList all = QStringList{QStringLiteral("*.wabcut")} + media + timelines + captions;
    return {
        tr("All supported (%1)").arg(all.join(space)),
        tr("Media (%1)").arg(media.join(space)),
        tr("Timelines (%1)").arg(timelines.join(space)),
        tr("Wabcut projects (*.wabcut)"),
        tr("All files (*)"),
    };
}

QStringList HomeBridge::projectNameFilters() const {
    return {tr("Wabcut projects (*.wabcut)"), tr("All files (*)")};
}

// ---- Recent projects ------------------------------------------------------------------------------------

void HomeBridge::load() {
    // The newest recentLimit projects of the stored history are listed (starred ones always): pruned().
    QVariantList stored = QSettings().value(kRecentKey).toList();
    stored.removeIf([](const QVariant& value) {
        return value.toMap().value(QStringLiteral("path")).toString().isEmpty();
    });
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale locale;
    QVariantList entries;
    for (const QVariant& value : pruned(stored)) {
        entries.append(publicEntry(value.toMap(), now, locale));
    }
    m_recent = entries;
}

void HomeBridge::store(const QVariantList& entries) {
    QSettings settings;
    settings.setValue(kRecentKey, entries);
    settings.sync();
    load();
    emit recentChanged();
    emit defaultFolderChanged();
}

namespace {

/// The newest `limit` entries, and the starred ones past them (the oldest unstarred ones go).
QVariantList prunedTo(QVariantList entries, qsizetype limit) {
    if (entries.size() <= limit) {
        return entries;
    }
    const QStringList starred = storedStarred();
    for (qsizetype i = entries.size() - 1; i >= 0 && entries.size() > limit; --i) {
        if (!containsPath(starred, entries[i].toMap().value(QStringLiteral("path")).toString())) {
            entries.removeAt(i);
        }
    }
    return entries;
}

} // namespace

QVariantList HomeBridge::pruned(QVariantList entries) const {
    return prunedTo(std::move(entries), recentLimit());
}

void HomeBridge::refreshRecent() {
    load();
    emit recentChanged();
}

void HomeBridge::removeRecent(const QUrl& file) {
    const QString path = localPath(file);
    const QVariantList stored = QSettings().value(kRecentKey).toList();
    QVariantList kept;
    m_removed.clear();
    for (qsizetype i = 0; i < stored.size(); ++i) {
        if (samePath(stored[i].toMap().value(QStringLiteral("path")).toString(), path)) {
            m_removed.append({static_cast<int>(i), stored[i]});
        } else {
            kept.append(stored[i]);
        }
    }
    store(kept);
}

void HomeBridge::clearRecent() {
    const QVariantList stored = QSettings().value(kRecentKey).toList();
    m_removed.clear();
    for (qsizetype i = 0; i < stored.size(); ++i) {
        m_removed.append({static_cast<int>(i), stored[i]});
    }
    store({});
}

int HomeBridge::clearUnstarred(const QStringList& starredPaths) {
    const QVariantList stored = QSettings().value(kRecentKey).toList();
    QVariantList kept;
    QList<QPair<int, QVariant>> removed;
    for (qsizetype i = 0; i < stored.size(); ++i) {
        if (containsPath(starredPaths, stored[i].toMap().value(QStringLiteral("path")).toString())) {
            kept.append(stored[i]);
        } else {
            removed.append({static_cast<int>(i), stored[i]});
        }
    }
    if (removed.isEmpty()) {
        return 0;
    }
    m_removed = removed;
    store(kept);
    return static_cast<int>(removed.size());
}

bool HomeBridge::undoRemoveRecent() {
    if (m_removed.isEmpty()) {
        return false;
    }
    QVariantList entries = QSettings().value(kRecentKey).toList();
    for (const auto& [index, entry] : std::as_const(m_removed)) { // in their old order: each lands where it was
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        const bool listed = std::ranges::any_of(entries, [&](const QVariant& e) {
            return samePath(e.toMap().value(QStringLiteral("path")).toString(), path);
        });
        if (!listed) {
            entries.insert(std::clamp<qsizetype>(index, 0, entries.size()), entry);
        }
    }
    m_removed.clear();
    store(entries);
    return true;
}

QVariantList HomeBridge::projectEntries(const QStringList& paths) const {
    QVariantList out;
    QStringList done;
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale locale;
    for (const QString& given : paths) {
        const QString path = given.startsWith(QLatin1String("file:")) ? QUrl(given).toLocalFile() : given;
        if (path.isEmpty() || containsPath(done, path)) {
            continue;
        }
        done << path;
        const auto listed = std::ranges::find_if(m_recent, [&](const QVariant& e) {
            return samePath(e.toMap().value(QStringLiteral("path")).toString(), path);
        });
        out.append(listed != m_recent.end() ? *listed : QVariant(publicEntry({{QStringLiteral("path"), path}}, now, locale)));
    }
    return out;
}

void HomeBridge::rememberProject(const QUrl& file) {
    const QString path = QFileInfo(localPath(file)).absoluteFilePath();
    if (path.isEmpty()) {
        return;
    }
    QVariantList entries = QSettings().value(kRecentKey).toList();
    entries.removeIf([&](const QVariant& e) {
        return samePath(e.toMap().value(QStringLiteral("path")).toString(), path);
    });
    const bool current = m_lifecycle && samePath(QDir::fromNativeSeparators(m_lifecycle->filePath()), path);
    entries.prepend(QVariantMap{
        {QStringLiteral("path"), path},
        {QStringLiteral("opened"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {QStringLiteral("format"), current ? currentFormat() : QString()},
        {QStringLiteral("thumbnail"), current ? currentThumbnail() : QString()},
    });
    // The history keeps up to the largest limit (the oldest unstarred ones beyond it go; starred ones stay), so
    // lowering Settings > General's "Recent projects to show" hides projects without forgetting them (load lists
    // recentLimit of them).
    store(prunedTo(entries, kMaxRecentLimit));
}

QVariantMap HomeBridge::locateProject(const QUrl& oldFile, const QUrl& newFile) {
    QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QString()}};
    const QFileInfo found(localPath(newFile));
    if (!found.isFile() || found.suffix().compare(QLatin1String("wabcut"), Qt::CaseInsensitive) != 0) {
        out.insert(QStringLiteral("error"), tr("Choose a Wabcut project (.wabcut)"));
        return out;
    }
    const QString oldPath = QFileInfo(localPath(oldFile)).absoluteFilePath();
    const QString newPath = found.absoluteFilePath();
    QVariantList entries = QSettings().value(kRecentKey).toList();
    qsizetype at = -1;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (samePath(entries[i].toMap().value(QStringLiteral("path")).toString(), oldPath)) {
            at = i;
            break;
        }
    }
    // Not in the list: a starred project that left it (Clear Recent, or the old limit of 12), which the Starred filter
    // lists from its star (projectEntries). Nothing to change here: the star moves to the file found (QML), and that
    // entry is the project found.
    if (at >= 0) {
        QVariantMap entry = entries[at].toMap();
        entry.insert(QStringLiteral("path"), newPath);
        entries[at] = entry;
        // The project found may be listed already: one entry for it, where the missing one was.
        for (qsizetype i = entries.size() - 1; i >= 0; --i) {
            if (i != at && samePath(entries[i].toMap().value(QStringLiteral("path")).toString(), newPath)) {
                entries.removeAt(i);
                if (i < at) {
                    --at;
                }
            }
        }
        m_removed.clear();
        store(entries);
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("url"), QUrl::fromLocalFile(newPath));
    out.insert(QStringLiteral("path"), QDir::toNativeSeparators(newPath));
    out.insert(QStringLiteral("name"), found.completeBaseName());
    out.insert(QStringLiteral("oldPath"), QDir::toNativeSeparators(oldPath));
    return out;
}

QUrl HomeBridge::existingFolder(const QUrl& file) const {
    const QString path = localPath(file);
    QString folder = path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
    for (int up = 0; up < 128 && !folder.isEmpty(); ++up) {
        if (QFileInfo(folder).isDir()) {
            return QUrl::fromLocalFile(folder);
        }
        const QString parent = QFileInfo(folder).absolutePath();
        if (parent == folder) {
            break;
        }
        folder = parent;
    }
    return defaultFolder();
}

bool HomeBridge::revealProject(const QUrl& file) const {
    const QFileInfo info(localPath(file));
    if (!info.exists()) {
        return false;
    }
    return LibraryBridge::revealPath(info.absoluteFilePath());
}

QString HomeBridge::copyPath(const QUrl& file) const {
    const QString path = localPath(file);
    if (path.isEmpty()) {
        return {};
    }
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        if (QClipboard* clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(native);
        }
    }
    return native;
}

void HomeBridge::onDocumentChanged() {
    if (!m_lifecycle || m_lifecycle->demo() || m_lifecycle->untitled()) {
        m_lastPath.clear();
        return;
    }
    const QString path = QDir::fromNativeSeparators(m_lifecycle->filePath());
    if (path.isEmpty()) {
        return;
    }
    if (!samePath(path, m_lastPath)) {
        m_lastPath = path;
        rememberProject(QUrl::fromLocalFile(path));
        return;
    }
    // The same project changed (saved, edited): keep its thumbnail and format up to date, in place.
    QVariantList entries = QSettings().value(kRecentKey).toList();
    for (QVariant& value : entries) {
        QVariantMap e = value.toMap();
        if (!samePath(e.value(QStringLiteral("path")).toString(), path)) {
            continue;
        }
        const QString thumbnail = currentThumbnail();
        const QString format = currentFormat();
        if (e.value(QStringLiteral("thumbnail")).toString() == thumbnail &&
            e.value(QStringLiteral("format")).toString() == format) {
            return;
        }
        e.insert(QStringLiteral("thumbnail"), thumbnail);
        e.insert(QStringLiteral("format"), format);
        value = e;
        store(entries);
        return;
    }
}

QString HomeBridge::currentThumbnail() const {
    if (!m_controller) {
        return {};
    }
    const ProjectSnapshot snapshot = m_controller->snapshot();
    if (!snapshot) {
        return {};
    }
    for (const MediaPtr& media : snapshot->media) {
        if (!media->info.hasVideo() || media->info.isStillImage || media->locator.offline) {
            continue;
        }
        if (const auto path = resolveMediaPath(media->locator, m_controller->projectFile())) {
            return ThumbnailImageProvider::urlFor(pathToUtf8(*path));
        }
    }
    return {};
}

QString HomeBridge::currentFormat() const {
    return m_controller ? m_controller->formatText() : QString();
}

// ---- Import sources (before the UX rework) and links ----------------------------------------------------

QVariantList HomeBridge::importSources() const {
    const QString all = tr("All files (*)");
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("premiere")},
                    {QStringLiteral("name"), tr("Premiere Pro")},
                    {QStringLiteral("steps"), tr("In Premiere: File › Export › Final Cut Pro XML")},
                    {QStringLiteral("formats"), tr("XML · .prproj (experimental)")},
                    {QStringLiteral("nameFilters"),
                     QStringList{tr("Premiere Pro timelines (*.xml *.prproj)"), all}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("resolve")},
                    {QStringLiteral("name"), tr("DaVinci Resolve")},
                    {QStringLiteral("steps"), tr("In Resolve: File › Export › Timeline › FCPXML or OTIO")},
                    {QStringLiteral("formats"), tr("FCPXML · OTIO · EDL")},
                    {QStringLiteral("nameFilters"),
                     QStringList{tr("Resolve timelines (*.fcpxml *.otio *.xml *.edl)"), all}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("finalcut")},
                    {QStringLiteral("name"), tr("Final Cut Pro")},
                    {QStringLiteral("steps"), tr("In Final Cut Pro: File › Export XML…")},
                    {QStringLiteral("formats"), tr("FCPXML")},
                    {QStringLiteral("nameFilters"), QStringList{tr("Final Cut Pro XML (*.fcpxml *.xml)"), all}}},
    };
}

QStringList HomeBridge::timelineNameFilters() const {
    return {tr("Timelines (*.xml *.fcpxml *.otio *.edl *.prproj)"), tr("All files (*)")};
}

QUrl HomeBridge::websiteUrl() const {
    return QUrl(kWebsite);
}

QUrl HomeBridge::guideUrl(const QString& page) const {
    const QString trimmed = page.trimmed();
    if (trimmed.startsWith(QLatin1String("https://")) || trimmed.startsWith(QLatin1String("http://"))) {
        return QUrl(trimmed);
    }
    if (trimmed.startsWith(QLatin1Char('/'))) {
        return QUrl(kWebsite + trimmed);
    }
    if (trimmed.isEmpty() || trimmed == QLatin1String("guide")) {
        return QUrl(kWebsite + QStringLiteral("/guide"));
    }
    return QUrl(kWebsite + QStringLiteral("/guide/") + trimmed);
}

QVariantList HomeBridge::links() const {
    struct Link {
        const char* id;
        const char* group;
        QString title;
        QString detail;
        const char* page; ///< a User Guide page (guideUrl), or a full URL
    };
    const std::array links{
        Link{"gettingStarted", "guide", tr("Getting started"), tr("Install, first run, first edit"), "first-run"},
        Link{"theWindow", "guide", tr("Tour the window"), tr("Panels, workspaces and menus"), "the-window"},
        Link{"fromPremiere", "guide", tr("Coming from Premiere Pro"), tr("The same keys, panels and names"),
             "coming-from-premiere"},
        Link{"exchangeTimelines", "guide", tr("Move timelines between editors"), tr("Premiere Pro, Resolve, Final Cut Pro"),
             "exchange-timelines"},
        Link{"shortcuts", "guide", tr("Keyboard shortcuts"), tr("Every key, and how to change it"), "keyboard-shortcuts"},
        Link{"gamingVideo", "guide", tr("Edit a long gaming video"), tr("Game, mic and voice chat on their own tracks"),
             "gaming-video"},
        Link{"shorts", "guide", tr("Make a Short"), tr("Vertical 9:16 for Shorts, TikTok and Reels"), "shorts"},
        Link{"videoEssay", "guide", tr("Make a video essay"), tr("Script, voice-over and B-roll"), "video-essay"},
        Link{"documentary", "guide", tr("Edit a documentary"), tr("Interviews, transcripts and captions"), "documentary"},
        Link{"allGuides", "guide", tr("All guides"), tr("The Wabcut User Guide"), ""},
        Link{"releaseNotes", "support", tr("Release notes"), tr("What changed in each version"), "release-notes"},
        Link{"roadmap", "support", tr("Roadmap"), tr("What comes next"), "roadmap"},
        Link{"reportIssue", "support", tr("Report an issue"), tr("On GitHub"), "https://github.com/sorsama/wabcut/issues"},
    };
    QVariantList out;
    for (const Link& link : links) {
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(link.id)},
                               {QStringLiteral("group"), QString::fromLatin1(link.group)},
                               {QStringLiteral("title"), link.title},
                               {QStringLiteral("detail"), link.detail},
                               {QStringLiteral("url"), guideUrl(QString::fromLatin1(link.page))}});
    }
    return out;
}

} // namespace wabcut::app
