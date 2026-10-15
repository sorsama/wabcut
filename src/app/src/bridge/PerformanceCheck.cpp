#include "PerformanceCheck.h"

#include "MediaReadinessBridge.h"

#include <QDateTime>
#include <QLocale>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace wabcut::app {

namespace {

const QString kReportKey = QStringLiteral("performance/report");
const QString kTimeKey = QStringLiteral("performance/time");
const QString kOfferedKey = QStringLiteral("performance/offered");

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QVariantMap row(const QString& key, const QString& label, const QString& value, bool good) {
    return {{QStringLiteral("key"), key}, {QStringLiteral("label"), label}, {QStringLiteral("value"), value},
            {QStringLiteral("good"), good}};
}

} // namespace

PerformanceCheck::PerformanceCheck(MediaReadinessBridge* readiness, QObject* parent)
    : QObject(parent), m_readiness(readiness) {
    QSettings settings;
    if (auto stored = editor::reportFromJson(settings.value(kReportKey).toString().toStdString())) {
        m_report = std::move(*stored);
        m_reportTime = settings.value(kTimeKey).toString();
    }
    if (m_readiness) {
        connect(m_readiness, &MediaReadinessBridge::settingsChanged, this, &PerformanceCheck::appliedChanged);
        connect(m_readiness, &MediaReadinessBridge::previewChanged, this, &PerformanceCheck::appliedChanged);
    }
}

PerformanceCheck::~PerformanceCheck() {
    // Stops and joins the probe; its queued results die with this object.
    m_worker = {};
}

PerformanceCheck* PerformanceCheck::create(QQmlEngine* qmlEngine, QJSEngine*) {
    return new PerformanceCheck(qmlEngine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness"));
}

void PerformanceCheck::setGraphics(const QString& graphics) {
    if (graphics == m_graphics) {
        return;
    }
    m_graphics = graphics;
    emit graphicsChanged();
}

bool PerformanceCheck::offerAtStartup() const {
    return !QSettings().value(kOfferedKey, false).toBool();
}

void PerformanceCheck::markOffered() {
    if (offerAtStartup()) {
        QSettings().setValue(kOfferedKey, true);
        emit offeredChanged();
    }
}

void PerformanceCheck::start() {
    if (m_running) {
        return;
    }
    markOffered();
    m_running = true;
    m_progress = 0.0;
    m_step = tr("Starting");
    emit runningChanged();
    emit progressChanged();
    m_worker = std::jthread([this, options = m_options](std::stop_token stop) {
        editor::PerformanceReport report =
            editor::runPerformanceProbe(options, stop, [this](double fraction, std::string_view step) {
                QMetaObject::invokeMethod(
                    this,
                    [this, fraction, text = QString::fromUtf8(step.data(), static_cast<qsizetype>(step.size()))] {
                        m_progress = fraction;
                        m_step = text;
                        emit progressChanged();
                    },
                    Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(this, [this, report = std::move(report)]() mutable { finish(std::move(report)); },
                                  Qt::QueuedConnection);
    });
}

void PerformanceCheck::cancel() {
    if (m_running) {
        m_worker.request_stop();
    }
}

void PerformanceCheck::finish(editor::PerformanceReport report) {
    m_worker = {}; // joins (the probe has returned)
    report.graphics = m_graphics.toStdString();
    m_report = std::move(report);
    m_reportTime = QDateTime::currentDateTime().toString(Qt::ISODate);
    QSettings settings;
    settings.setValue(kReportKey, QString::fromStdString(editor::toJson(*m_report)));
    settings.setValue(kTimeKey, m_reportTime);
    m_running = false;
    m_progress = 1.0;
    emit progressChanged();
    emit runningChanged();
    emit reportChanged();
    emit appliedChanged();
    emit finished();
}

void PerformanceCheck::apply() {
    if (!m_report) {
        return;
    }
    const editor::ReadinessRecommendation& r = m_report->recommendation;
    if (m_readiness) {
        m_readiness->setPolicy(qs(r.proxyPolicy));
        m_readiness->setProxyFormat(qs(r.proxyFormat));
        m_readiness->setProxyDivisor(r.proxyDivisor);
        m_readiness->setPreviewQuality(qs(r.previewQuality));
    } else { // no bridge (tests): MediaReadiness reads these at startup
        QSettings settings;
        settings.setValue(QStringLiteral("proxies/policy"), qs(r.proxyPolicy));
        settings.setValue(QStringLiteral("proxies/format"), qs(r.proxyFormat));
        settings.setValue(QStringLiteral("proxies/divisor"), r.proxyDivisor);
        settings.setValue(QStringLiteral("preview/quality"), qs(r.previewQuality));
    }
    emit appliedChanged();
}

bool PerformanceCheck::applied() const {
    if (!m_report) {
        return false;
    }
    const editor::ReadinessRecommendation& r = m_report->recommendation;
    if (m_readiness) {
        return m_readiness->policy() == qs(r.proxyPolicy) && m_readiness->proxyFormat() == qs(r.proxyFormat) &&
               m_readiness->proxyDivisor() == r.proxyDivisor && m_readiness->previewQuality() == qs(r.previewQuality);
    }
    QSettings settings;
    return settings.value(QStringLiteral("proxies/policy"), QStringLiteral("auto")).toString() == qs(r.proxyPolicy) &&
           settings.value(QStringLiteral("proxies/format"), QStringLiteral("prores")).toString() == qs(r.proxyFormat) &&
           settings.value(QStringLiteral("proxies/divisor"), 2).toInt() == r.proxyDivisor &&
           settings.value(QStringLiteral("preview/quality"), QStringLiteral("auto")).toString() == qs(r.previewQuality);
}

void PerformanceCheck::skip() {
    markOffered();
}

void PerformanceCheck::openCheck() {
    emit openRequested();
}

QString PerformanceCheck::reportTime() const {
    const QDateTime when = QDateTime::fromString(m_reportTime, Qt::ISODate);
    return when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : QString();
}

QVariantList PerformanceCheck::rows() const {
    QVariantList rows;
    if (!m_report) {
        return rows;
    }
    const editor::PerformanceReport& r = *m_report;
    const double realTime = r.frameRate > 0.0 ? r.frameRate : 30.0;
    for (const editor::DecodeMeasurement& m : r.decode) {
        //: %1 is a codec (H.264, HEVC, ProRes)
        const QString label = tr("%1 playback").arg(qs(m.codec));
        if (!m.available || m.bestFps() <= 0.0) {
            //: %1 says why (an English note from the check)
            rows.push_back(row(qs(m.codec), label, tr("Not measured (%1)").arg(qs(m.note)), false));
            continue;
        }
        const double factor = m.bestFps() / realTime;
        const QString how = m.hardwareBackend.empty() || m.softwareFps > m.hardwareFps
                                ? tr("software")
                                //: %1 is a hardware decoder (d3d11va, cuda, videotoolbox, ...)
                                : tr("hardware, %1").arg(qs(m.hardwareBackend));
        //: %1 is how many times real time (e.g. 6.2), %2 hardware/software
        rows.push_back(row(qs(m.codec), label,
                           tr("%1x real time (%2)").arg(QLocale().toString(factor, 'f', 1), how), factor >= 1.0));
    }
    QStringList encoders;
    bool anyEncoder = false;
    for (const editor::EncoderMeasurement& e : r.encoders) {
        if (e.encoder.empty()) {
            //: %1 is a codec: no encoder works for it
            encoders << tr("%1: none").arg(qs(e.codec));
            continue;
        }
        anyEncoder = true;
        //: %1 codec, %2 FFmpeg encoder name, e.g. "H.264: h264_nvenc (GPU)"
        encoders << (e.hardware ? tr("%1: %2 (GPU)") : tr("%1: %2 (CPU)")).arg(qs(e.codec), qs(e.encoder));
    }
    if (!r.encoders.empty()) {
        rows.push_back(row(QStringLiteral("encoders"), tr("Export"), encoders.join(QStringLiteral(" · ")), anyEncoder));
    }
    QString graphics = qs(r.graphics);
    if (graphics.isEmpty()) {
        QStringList gpus;
        for (const std::string& gpu : r.gpus) {
            gpus << qs(gpu);
        }
        graphics = gpus.isEmpty() ? tr("No GPU found") : gpus.join(QStringLiteral(", "));
    }
    rows.push_back(row(QStringLiteral("graphics"), tr("Graphics"), graphics, !r.gpus.empty() || !r.graphics.empty()));
    rows.push_back(row(QStringLiteral("audio"), tr("Audio output"),
                       r.audioDevice ? qs(r.audioName) : tr("No output device found"), r.audioDevice));
    if (r.cpuThreads > 0) {
        rows.push_back(row(QStringLiteral("cpu"), tr("Processor"), tr("%n thread(s)", nullptr, r.cpuThreads), true));
    }
    return rows;
}

QStringList PerformanceCheck::reasons() const {
    QStringList list;
    if (m_report) {
        for (const std::string& reason : m_report->recommendation.reasons) {
            list << qs(reason);
        }
    }
    return list;
}

QVariantMap PerformanceCheck::recommendation() const {
    if (!m_report) {
        return {};
    }
    const editor::ReadinessRecommendation& r = m_report->recommendation;
    const QString policy = r.proxyPolicy == "always" ? tr("for every video")
                           : r.proxyPolicy == "off"  ? tr("off")
                                                     : tr("when media needs them");
    const QString format = r.proxyFormat == "h264"    ? tr("H.264")
                           : r.proxyFormat == "dnxhr" ? tr("DNxHR LB")
                                                      : tr("ProRes Proxy");
    const QString size = r.proxyDivisor == 4 ? tr("1/4 size") : tr("1/2 size");
    const QString preview = r.previewQuality == "half"      ? QStringLiteral("1/2")
                            : r.previewQuality == "quarter" ? QStringLiteral("1/4")
                            : r.previewQuality == "full"    ? tr("Full")
                                                            : tr("Auto");
    //: %1 when proxies are made, %2 their format, %3 their size, %4 the preview quality
    const QString text = tr("Proxies %1 (%2, %3) · Preview quality %4").arg(policy, format, size, preview);
    return {{QStringLiteral("proxyPolicy"), qs(r.proxyPolicy)},
            {QStringLiteral("proxyFormat"), qs(r.proxyFormat)},
            {QStringLiteral("proxyDivisor"), r.proxyDivisor},
            {QStringLiteral("previewQuality"), qs(r.previewQuality)},
            {QStringLiteral("text"), text}};
}

} // namespace wabcut::app
