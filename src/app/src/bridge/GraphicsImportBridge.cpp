#include "GraphicsImportBridge.h"

#include "BrandBridge.h"
#include "BridgeIds.h"
#include "ProjectController.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QQmlEngine>

#include <cmath>

namespace wabcut::app {

using command::Json;

namespace {

/// "30" for 30/1, "29.97" for 30000/1001.
QString rateText(const std::string& fraction) {
    const auto slash = fraction.find('/');
    if (slash == std::string::npos)
        return QString::fromStdString(fraction);
    const double num = std::atof(fraction.substr(0, slash).c_str());
    const double den = std::atof(fraction.substr(slash + 1).c_str());
    if (den <= 0)
        return QString::fromStdString(fraction);
    const double fps = num / den;
    return QLocale().toString(fps, 'f', std::abs(fps - std::round(fps)) < 0.005 ? 0 : 2);
}

QString text(const Json& data, const char* key) {
    const auto it = data.find(key);
    return it != data.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
}

} // namespace

GraphicsImportBridge::GraphicsImportBridge(QObject* parent) : QObject(parent) {}

GraphicsImportBridge* GraphicsImportBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GraphicsImportBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void GraphicsImportBridge::setController(ProjectController* controller) { m_controller = controller; }

QStringList GraphicsImportBridge::nameFilters() const {
    return {tr("Lottie animations (*.json *.lottie)"), tr("All files (*)")};
}

void GraphicsImportBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

bool GraphicsImportBridge::isLottie(const QUrl& url) const {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("lottie"))
        return true; // a dotLottie archive: claimed so that it is refused with a reason, not imported as media
    if (suffix != QLatin1String("json"))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return looksLikeLottie(file.read(64 * 1024));
}

QString GraphicsImportBridge::importLottie(const QUrl& url, const QVariantMap& options) {
    if (!m_controller || !m_controller->currentSequence()) {
        const QString error = tr("Open a sequence to import an animation into");
        setMessage(error);
        return error;
    }
    if (m_job.running()) {
        // A drop of several files: after the one importing, each after the clip before it.
        m_queue.append({url, options});
        return {};
    }
    return start(url, options, m_controller->playhead());
}

QString GraphicsImportBridge::start(const QUrl& url, const QVariantMap& options, Ticks at) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String("lottie"), Qt::CaseInsensitive) == 0) {
        //: dotLottie is a zip format of LottieFiles; Bodymovin is After Effects' Lottie exporter
        const QString error = tr("%1 is a dotLottie archive: export the animation as Lottie JSON (Bodymovin) to import it")
                                  .arg(info.fileName());
        setMessage(error);
        return error;
    }
    Json params{{"path", path.toStdString()}, {"at", at.value}};
    if (options.value(QStringLiteral("saveAsTemplate")).toBool()) {
        // A template id from the file name: [A-Za-z0-9][A-Za-z0-9_.-]*.
        QString id;
        for (const QChar c : info.completeBaseName()) {
            if (c.isLetterOrNumber() && c.unicode() < 128)
                id += c;
            else if (!id.isEmpty() && (c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.')))
                id += c;
        }
        if (id.isEmpty())
            id = QStringLiteral("lottie");
        params["saveAs"] = Json{{"id", ("lottie." + id).toStdString()},
                                {"name", options.value(QStringLiteral("name"), info.completeBaseName()).toString().toStdString()}};
    }
    setMessage({});
    const QString fileName = info.fileName();
    QPointer<GraphicsImportBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, nullptr, {"lottie.import", std::move(params), tr("Import Lottie").toStdString()},
        [](const QString&, double) {},
        [self, fileName, at](const command::CommandResult& result) {
            if (!self)
                return;
            emit self->busyChanged();
            self->m_nextAt = at;
            if (!result.ok()) {
                self->setMessage(commandErrorText(result));
            } else {
                if (self->m_controller) {
                    if (const auto it = result.data.find("clip"); it != result.data.end() && it->is_number_integer()) {
                        const ClipId clip{it->get<std::uint64_t>()};
                        self->m_controller->selectClip(clipKey(clip));
                        if (const SequencePtr seq = self->m_controller->currentSequence()) {
                            if (const auto loc = seq->findClip(clip))
                                self->m_nextAt = loc->clip().end();
                        }
                    }
                }
                emit self->imported(reportMap(result.data, fileName));
            }
            // The next file dropped with this one.
            if (!self->m_queue.isEmpty()) {
                const auto [url, options] = self->m_queue.takeFirst();
                const Ticks next = self->m_nextAt;
                QMetaObject::invokeMethod(self, [self, url, options, next] {
                    if (self)
                        self->start(url, options, next);
                }, Qt::QueuedConnection);
            }
        });
    if (!started) {
        m_queue.append({url, options});
        return {};
    }
    emit busyChanged();
    return {};
}

QVariantMap GraphicsImportBridge::reportMap(const Json& data, const QString& fileName) {
    const Json& report = data.contains("report") ? data["report"] : Json::object();
    QVariantList entries;
    if (report.contains("entries") && report["entries"].is_array()) {
        for (const Json& e : report["entries"]) {
            entries.append(QVariantMap{{QStringLiteral("outcome"), text(e, "outcome")},
                                       {QStringLiteral("category"), text(e, "category")},
                                       {QStringLiteral("reason"), text(e, "reason")},
                                       {QStringLiteral("location"), text(e, "element")}});
        }
    }
    const int width = data.value("width", 0);
    const int height = data.value("height", 0);
    const double seconds = static_cast<double>(data.value("duration", std::int64_t{0})) / static_cast<double>(kTicksPerSecond);
    QStringList summary;
    //: The imported animation's design canvas, e.g. "1920 × 1080"
    summary << tr("%1 × %2").arg(width).arg(height);
    //: The animation's length in seconds, e.g. "3.0 s"
    summary << tr("%1 s").arg(QLocale().toString(seconds, 'f', 1));
    //: Frames per second, e.g. "30 fps"
    summary << tr("%1 fps").arg(rateText(data.value("frameRate", std::string())));
    if (const QString start = text(data, "start"); !start.isEmpty())
        //: Where the imported animation was placed on the timeline
        summary << tr("a graphic clip at %1").arg(start);
    if (const QString tmpl = text(data, "template"); !tmpl.isEmpty())
        summary << tr("saved as a template");
    return {{QStringLiteral("file"), fileName},
            //: The file format of an imported animation
            {QStringLiteral("format"), tr("Lottie")},
            {QStringLiteral("summary"), tr("Lottie animation · %1").arg(summary.join(QStringLiteral(" · ")))},
            {QStringLiteral("carried"), report.value("carried", 0)},
            {QStringLiteral("approximated"), report.value("approximated", 0)},
            {QStringLiteral("dropped"), report.value("dropped", 0)},
            {QStringLiteral("entries"), entries}};
}

} // namespace wabcut::app
