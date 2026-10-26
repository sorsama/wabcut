#pragma once

// PerformanceCheck: the first-run performance check (singleton `PerformanceCheck`; I.7, docs/release.md "First-run
// performance check"). Owner: Graphics (integration). About ten seconds, optional and skippable: editor::
// runPerformanceProbe() measures decoding (H.264, HEVC, ProRes; hardware and software), the export encoders, the GPUs
// and the audio output on a worker thread, and recommends media-readiness settings (proxy policy, format and size,
// preview quality). The recommendation is applied only when the user accepts it, through MediaReadiness (the same
// settings as Settings > Media & cache).
//
// Offered once, after the launch Home screen and the welcome (Main.qml; qml/dialogs/FirstRunCheckDialog.qml); Settings
// > Media & cache runs it again (openCheck()). The last report is kept in the settings ("performance/report", JSON) and
// shown until the next run.

#include <wabcut/editor/PerformanceProbe.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class MediaReadinessBridge;

class PerformanceCheck : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PerformanceCheck)
    QML_SINGLETON

    Q_PROPERTY(bool running READ running NOTIFY runningChanged FINAL)
    /// 0..1 while running.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    /// What it measures now ("Decoding HEVC (hardware)").
    Q_PROPERTY(QString step READ step NOTIFY progressChanged FINAL)
    /// A report exists (this run's or the last one kept in the settings).
    Q_PROPERTY(bool hasReport READ hasReport NOTIFY reportChanged FINAL)
    /// "12 Oct 2026 10:20", when the report was made.
    Q_PROPERTY(QString reportTime READ reportTime NOTIFY reportChanged FINAL)
    /// The summary's rows: [{ key, label, value, good }] (decoding per codec, encoders, graphics, the GPU in use and why
    /// - key "gpu", issue #10 - audio, CPU).
    Q_PROPERTY(QVariantList rows READ rows NOTIFY reportChanged FINAL)
    /// One sentence per decision.
    Q_PROPERTY(QStringList reasons READ reasons NOTIFY reportChanged FINAL)
    /// { proxyPolicy, proxyFormat, proxyDivisor, previewQuality } and `text`, the settings in words.
    Q_PROPERTY(QVariantMap recommendation READ recommendation NOTIFY reportChanged FINAL)
    /// The check was cancelled or ran out of time: the report is partial.
    Q_PROPERTY(bool partial READ partial NOTIFY reportChanged FINAL)
    /// The current media-readiness settings are the recommendation.
    Q_PROPERTY(bool applied READ applied NOTIFY appliedChanged FINAL)
    /// Not offered (run or skipped) yet: Main.qml offers it once after the launch Home screen and the welcome.
    Q_PROPERTY(bool offerAtStartup READ offerAtStartup NOTIFY offeredChanged FINAL)
    /// The graphics API and adapter (RendererInfo.summary), added to the report.
    Q_PROPERTY(QString graphics READ graphics WRITE setGraphics NOTIFY graphicsChanged FINAL)

public:
    PerformanceCheck(MediaReadinessBridge* readiness, QObject* parent = nullptr);
    ~PerformanceCheck() override;
    static PerformanceCheck* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- C++ (tests) ----
    void setOptions(const editor::ProbeOptions& options) { m_options = options; }
    const std::optional<editor::PerformanceReport>& report() const { return m_report; }

    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString step() const { return m_step; }
    bool hasReport() const { return m_report.has_value(); }
    QString reportTime() const;
    QVariantList rows() const;
    QStringList reasons() const;
    QVariantMap recommendation() const;
    bool partial() const { return m_report && (m_report->cancelled || m_report->outOfTime); }
    bool applied() const;
    bool offerAtStartup() const;
    QString graphics() const { return m_graphics; }
    void setGraphics(const QString& graphics);

    // ---- QML ----
    /// Runs the check in the background (no effect while it runs). Counts as offered.
    Q_INVOKABLE void start();
    /// Ends it early: what was measured is kept and recommended from.
    Q_INVOKABLE void cancel();
    /// Applies the recommendation to the media-readiness settings.
    Q_INVOKABLE void apply();
    /// "Not now": not offered at startup again (Settings > Media & cache still runs it).
    Q_INVOKABLE void skip();
    /// Opens the check's dialog (Settings > Media & cache).
    Q_INVOKABLE void openCheck();

signals:
    void runningChanged();
    void progressChanged();
    void reportChanged();
    void appliedChanged();
    void offeredChanged();
    void graphicsChanged();
    void openRequested();
    /// A run ended (finished or cancelled).
    void finished();

private:
    void markOffered();
    void finish(editor::PerformanceReport report);

    QPointer<MediaReadinessBridge> m_readiness;
    editor::ProbeOptions m_options;
    std::optional<editor::PerformanceReport> m_report;
    QString m_reportTime;
    QString m_graphics;
    bool m_running = false;
    double m_progress = 0.0;
    QString m_step;
    std::jthread m_worker;
};

} // namespace wabcut::app
