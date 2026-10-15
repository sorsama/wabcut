#include "CollectFilesBridge.h"

#include "CaptionStyleMapping.h"
#include "CaptionsBridge.h"
#include "MediaReadinessBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/ProxyManager.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <fstream>
#include <map>
#include <set>

namespace wabcut::app {

namespace {

std::filesystem::path toPath(const QString& local) {
    return std::filesystem::path(local.toStdU16String());
}

QString toQString(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}

QString sizeText(std::int64_t bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

/// Families that come with Windows: listed in the manifest, never copied (their licences do not allow it).
bool isWindowsFont(const QString& family) {
    static const QStringList kSystem = {
        QStringLiteral("Segoe"),     QStringLiteral("Arial"),     QStringLiteral("Times New Roman"),
        QStringLiteral("Georgia"),   QStringLiteral("Consolas"),  QStringLiteral("Cascadia"),
        QStringLiteral("Calibri"),   QStringLiteral("Cambria"),   QStringLiteral("Candara"),
        QStringLiteral("Leelawadee"), QStringLiteral("Tahoma"),   QStringLiteral("Verdana"),
        QStringLiteral("Courier New"), QStringLiteral("Microsoft"), QStringLiteral("Malgun"),
        QStringLiteral("Yu Gothic"), QStringLiteral("MS "),       QStringLiteral("Trebuchet"),
        QStringLiteral("Impact"),    QStringLiteral("Comic Sans"), QStringLiteral("Lucida"),
        QStringLiteral("Corbel"),    QStringLiteral("Constantia"), QStringLiteral("Ebrima"),
        QStringLiteral("Gadugi"),    QStringLiteral("Nirmala"),   QStringLiteral("SimSun"),
        QStringLiteral("Sylfaen"),   QStringLiteral("Symbol"),    QStringLiteral("Webdings"),
        QStringLiteral("Wingdings"), QStringLiteral("Bahnschrift"), QStringLiteral("Ink Free"),
        QStringLiteral("Javanese"),  QStringLiteral("Marlett"),   QStringLiteral("Mongolian"),
        QStringLiteral("Myanmar"),   QStringLiteral("Palatino"),  QStringLiteral("Sitka"),
    };
    return std::ranges::any_of(kSystem, [&](const QString& s) { return family.startsWith(s, Qt::CaseInsensitive); });
}

/// Font files of `family` from Windows' font registry (machine and per-user installs). Empty elsewhere.
std::vector<std::filesystem::path> fontFiles(const QString& family) {
    std::vector<std::filesystem::path> out;
#if defined(Q_OS_WIN)
    const QString fontsDir = QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))) +
                             QStringLiteral("/Fonts/");
    for (const QString& hive : {QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_CURRENT_USER")}) {
        QSettings reg(hive + QStringLiteral("\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"), QSettings::NativeFormat);
        for (const QString& key : reg.childKeys()) {
            // "Kanit Regular (TrueType)", "Kanit & Kanit Light (TrueType)": the family, then a space or the end.
            if (!key.startsWith(family, Qt::CaseInsensitive) ||
                (key.size() > family.size() && key[family.size()] != QLatin1Char(' '))) {
                continue;
            }
            QString file = reg.value(key).toString();
            if (QFileInfo(file).isRelative()) {
                file = fontsDir + file;
            }
            if (QFileInfo::exists(file)) {
                out.push_back(toPath(file));
            }
        }
    }
#else
    (void)family;
#endif
    return out;
}

} // namespace

struct CollectFilesBridge::Inputs {
    ProjectState state;
    std::filesystem::path projectFile;
    editor::CapsuleOptions options;
    std::filesystem::path tempDir;
};

CollectFilesBridge::CollectFilesBridge(QObject* parent) : QObject(parent) {
    m_gate->owner = this;
}

CollectFilesBridge::~CollectFilesBridge() {
    {
        std::lock_guard lock(m_gate->mutex);
        m_gate->owner = nullptr;
    }
    if (m_job.joinable()) {
        m_job.request_stop();
        m_job.join();
    }
    std::error_code ec;
    if (!m_tempDir.empty()) {
        std::filesystem::remove_all(m_tempDir, ec);
    }
}

CollectFilesBridge* CollectFilesBridge::create(QQmlEngine*, QJSEngine*) {
    return new CollectFilesBridge; // the other singletons are looked up on use (qmlEngine(this))
}

QUrl CollectFilesBridge::suggestedFolder() const {
    if (QQmlEngine* engine = qmlEngine(this)) {
        if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
            if (!file->document().file().empty()) {
                return QUrl::fromLocalFile(toQString(file->document().file().parent_path()));
            }
        }
    }
    return QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

QString CollectFilesBridge::suggestedName() const {
    if (QQmlEngine* engine = qmlEngine(this)) {
        if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
            return file->name() + tr(" Capsule");
        }
    }
    return tr("Project Capsule");
}

QVariantList CollectFilesBridge::fontsInUse() const {
    QVariantList out;
    QQmlEngine* engine = qmlEngine(this);
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!file) {
        return out;
    }
    std::set<QString> families;
    const ProjectSnapshot snap = file->document().session().snapshot();
    for (const MediaPtr& m : snap->media) {
        if (m && m->generator && !m->generator->fontFamily.empty()) {
            families.insert(QString::fromStdString(m->generator->fontFamily));
        }
    }
    if (auto* captions = engine->singletonInstance<CaptionsBridge*>("Wabcut", "Captions")) {
        bool any = false;
        for (const SequencePtr& seq : snap->sequences) {
            any = any || (seq && !seq->captionTracks.empty());
        }
        if (any) {
            const std::string family = captionStyleFrom(captions->style()).fontFamily;
            if (!family.empty()) {
                families.insert(QString::fromStdString(family));
            }
        }
    }
    for (const QString& family : families) {
        QStringList files;
        for (const auto& f : fontFiles(family)) {
            files << toQString(f);
        }
        out.append(QVariantMap{{QStringLiteral("family"), family},
                               {QStringLiteral("files"), files},
                               {QStringLiteral("system"), isWindowsFont(family)}});
    }
    return out;
}

std::expected<CollectFilesBridge::Inputs, QString> CollectFilesBridge::gather(const QUrl& folder, const QVariantMap& o) {
    QQmlEngine* engine = qmlEngine(this);
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!file) {
        return std::unexpected(tr("There is no project to collect."));
    }
    Inputs in;
    in.state = file->document().session().stateForSave();
    in.projectFile = file->document().file();
    if (auto* readiness = engine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness")) {
        if (readiness->manager()) {
            readiness->manager()->applyTo(in.state); // the proxies the registry holds
        }
    }
    editor::CapsuleOptions& options = in.options;
    options.destination = toPath(folder.toLocalFile());
    options.name = o.value(QStringLiteral("name")).toString().trimmed().toStdString();
    options.trim = o.value(QStringLiteral("trim"), false).toBool();
    options.handles = Ticks{static_cast<std::int64_t>(o.value(QStringLiteral("handles"), 1.0).toDouble() * kTicksPerSecond)};
    options.includeProxies = o.value(QStringLiteral("proxies"), false).toBool();
    options.excludeUnused = !o.value(QStringLiteral("unused"), false).toBool();
    options.verify = true;
    options.wabcutVersion = QCoreApplication::applicationVersion().toStdString();
    if (o.value(QStringLiteral("fonts"), true).toBool()) {
        std::set<std::string> taken;
        for (const QVariant& v : fontsInUse()) {
            const QVariantMap font = v.toMap();
            if (font.value(QStringLiteral("system")).toBool()) {
                continue;
            }
            for (const QVariant& f : font.value(QStringLiteral("files")).toList()) {
                const std::filesystem::path source = toPath(f.toString());
                if (taken.insert(pathToUtf8(source.filename())).second) {
                    options.extras.push_back({source, std::filesystem::path("Fonts") / source.filename(), "font"});
                }
            }
        }
    }
    if (o.value(QStringLiteral("analysis"), false).toBool()) {
        // Analysis results (transcripts, shots, beats) as portable sidecars, one per media file.
        in.tempDir = std::filesystem::temp_directory_path() / ("wabcut-collect-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
        std::error_code ec;
        std::filesystem::create_directories(in.tempDir, ec);
        const ai::AnalysisCache cache(ai::AnalysisCache::defaultRoot());
        std::set<QString> taken; // case-folded: on Windows "A.analysis.json" and "a.analysis.json" are one file
        for (const MediaPtr& m : in.state.media) {
            if (!m || m->isGenerator() || m->locator.path.empty()) {
                continue;
            }
            const auto fp = media::sourceFingerprint(m->locator.path);
            if (!fp) {
                continue;
            }
            const auto sidecar = cache.exportSidecar(*fp);
            if (!sidecar || sidecar->empty()) {
                continue;
            }
            std::string name = pathToUtf8(pathFromUtf8(m->locator.path).stem()) + ".analysis.json";
            for (int n = 2; !taken.insert(QString::fromStdString(name).toCaseFolded()).second; ++n) {
                name = pathToUtf8(pathFromUtf8(m->locator.path).stem()) + " (" + std::to_string(n) + ").analysis.json";
            }
            const std::filesystem::path temp = in.tempDir / pathFromUtf8(name);
            std::ofstream(temp, std::ios::binary) << *sidecar;
            options.extras.push_back({temp, std::filesystem::path("Analysis") / pathFromUtf8(name), "analysis"});
        }
    }
    return in;
}

QString CollectFilesBridge::estimate(const QUrl& folder, const QVariantMap& options) {
    auto in = gather(folder, options);
    if (!in) {
        return in.error();
    }
    const std::int64_t bytes = editor::capsuleBytesNeeded(in->state, in->projectFile, in->options);
    std::error_code ec;
    const auto space = std::filesystem::space(in->options.destination, ec);
    if (!in->tempDir.empty()) {
        std::filesystem::remove_all(in->tempDir, ec);
    }
    //: Collect Files estimate, e.g. "About 1.4 GB (210 GB free)"
    return ec ? tr("About %1").arg(sizeText(bytes))
              : tr("About %1 (%2 free)").arg(sizeText(bytes), sizeText(static_cast<std::int64_t>(space.available)));
}

bool CollectFilesBridge::start(const QUrl& folder, const QVariantMap& options) {
    if (m_running) {
        return false;
    }
    auto in = gather(folder, options);
    if (!in) {
        m_result = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), in.error()}};
        emit stateChanged();
        return false;
    }
    if (m_job.joinable()) {
        m_job.join();
    }
    m_tempDir = in->tempDir;
    m_running = true;
    m_progress = 0.0;
    m_phase = tr("Checking free space");
    m_current.clear();
    m_result.clear();
    emit stateChanged();
    emit progressChanged();
    m_job = std::jthread([gate = m_gate, in = std::move(*in)](std::stop_token stop) {
        const auto post = [&gate](auto fn) {
            std::lock_guard lock(gate->mutex);
            if (CollectFilesBridge* owner = gate->owner) {
                QMetaObject::invokeMethod(owner, [owner, fn] { fn(owner); }, Qt::QueuedConnection);
            }
        };
        auto result = editor::collectProject(in.state, in.projectFile, in.options, stop,
                                             [&](const editor::CapsuleProgress& p) {
                                                 post([p](CollectFilesBridge* owner) { owner->onProgress(p); });
                                             });
        post([result = std::move(result)](CollectFilesBridge* owner) { owner->onDone(result); });
    });
    return true;
}

void CollectFilesBridge::onProgress(const editor::CapsuleProgress& p) {
    const QString phase = p.phase == "checking"    ? tr("Checking free space")
                          : p.phase == "copying"   ? tr("Copying")
                          : p.phase == "trimming"  ? tr("Trimming")
                          : p.phase == "saving"    ? tr("Saving the project")
                          : p.phase == "verifying" ? tr("Verifying the capsule")
                                                   : tr("Finishing");
    const double progress =
        p.bytesTotal > 0 ? std::clamp(static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal), 0.0, 1.0) : 0.0;
    const QString current = QString::fromStdString(p.current);
    if (phase != m_phase || current != m_current || std::abs(progress - m_progress) >= 0.005) {
        m_phase = phase;
        m_current = current;
        m_progress = progress;
        emit progressChanged();
    }
}

void CollectFilesBridge::onDone(const std::expected<editor::CapsuleResult, Error>& r) {
    m_running = false;
    std::error_code ec;
    if (!m_tempDir.empty()) {
        std::filesystem::remove_all(m_tempDir, ec);
        m_tempDir.clear();
    }
    if (r) {
        m_lastFolder = r->folder;
        QStringList notes;
        for (const std::string& n : r->notes) {
            notes << QString::fromStdString(n);
        }
        m_result = {
            {QStringLiteral("ok"), true},
            {QStringLiteral("cancelled"), false},
            {QStringLiteral("folder"), QUrl::fromLocalFile(toQString(r->folder))},
            {QStringLiteral("folderPath"), QDir::toNativeSeparators(toQString(r->folder))},
            {QStringLiteral("project"), QUrl::fromLocalFile(toQString(r->projectFile))},
            {QStringLiteral("verified"), r->verified},
            {QStringLiteral("offline"), r->offlineAfterReopen},
            {QStringLiteral("mismatches"), r->fingerprintMismatches},
            {QStringLiteral("files"), static_cast<int>(r->files.size())},
            {QStringLiteral("media"), r->mediaFiles},
            {QStringLiteral("trimmed"), r->trimmedFiles},
            {QStringLiteral("size"), sizeText(r->bytes)},
            {QStringLiteral("notes"), notes},
        };
    } else {
        const bool cancelled = r.error().code == ErrorCode::Cancelled;
        m_result = {{QStringLiteral("ok"), false},
                    {QStringLiteral("cancelled"), cancelled},
                    {QStringLiteral("error"), cancelled ? tr("Cancelled: nothing was left behind.")
                                                        : QString::fromStdString(r.error().message)}};
    }
    emit stateChanged();
    emit progressChanged();
    emit finished(r.has_value());
}

void CollectFilesBridge::cancel() {
    if (m_running) {
        m_job.request_stop();
    }
}

bool CollectFilesBridge::reveal() {
    if (m_lastFolder.empty()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(m_lastFolder)));
}

bool CollectFilesBridge::waitFinished(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (m_running) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

} // namespace wabcut::app
