#pragma once

// GamingExport (M5o, gaming): the Export panel's gaming presets and the size / time estimate (singleton
// `GamingExport`).
//
//   * The presets are app-side plans on top of the exporter's built-in presets (exporter::builtInPresets(), owned by
//     the export engine): a base preset plus ExportController overrides (size, bitrate, frame rate, audio). YouTube
//     1080p60 / 1440p60 / 4K60, Shorts / TikTok / Reels 1080x1920, Twitch 1080p60, Discord 10 / 25 / 50 MB, and GIF /
//     WebM, which stay unavailable ("Coming soon") until the exporter has those formats.
//   * Frame rates are capped, never raised: a 144 fps recording exports at 60, a 30 fps one stays at 30.
//   * Discord size targets: no two-pass encoding yet, so the video bitrate is (95 % of the target - the audio) / the
//     duration, a one-pass target with a tight cap, marked approximate. The estimate never exceeds the target; a range
//     too long for any picture is refused with the longest length that fits.
//   * estimate(): the file size (bitrate x duration, + audio, + 1 % container) and the encode time, from P.1's export
//     benchmark (tests/perf/baselines: H.264 / HEVC on NVENC, x264 and x265 at 1080p and 2160p, the whole render +
//     encode pipeline) scaled by pixels, with a conservative margin. Both are labelled estimates.
//
// Every value is computed from the project snapshot on the GUI thread (resolving an export is arithmetic).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <string>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ExportController;
class ProjectController;

class GamingExport : public QObject {
    Q_OBJECT
    /// [{ id, group, name, description, icon, available, reason }], grouped in order.
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    /// [{ id, name, icon }] in the order the panel shows them.
    Q_PROPERTY(QVariantList groups READ groups CONSTANT FINAL)

public:
    explicit GamingExport(QObject* parent = nullptr);
    static GamingExport* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller) { m_controller = controller; }
    void setExport(ExportController* exporter) { m_export = exporter; }

    QVariantList presets() const;
    QVariantList groups() const;

    /// The export a gaming preset makes of a sequence ("" = the current one) over `range` ("whole" | "inOut" |
    /// "workArea"): { ok, id, presetId (the built-in base), overrides (ExportController keys), notes: [text],
    /// available, reason, error }.
    Q_INVOKABLE QVariantMap plan(const QString& id, const QString& sequenceId, const QString& range) const;
    /// What an export writes, as an estimate: { ok, error, bytes, sizeText, seconds, durationText, encodeSeconds,
    /// encodeText, encoder, encoderText, hardware, videoMbps, audioKbps, width, height, fpsText, approximate,
    /// notes: [text] }. Works for every preset (built-in or gaming) and the panel's overrides.
    Q_INVOKABLE QVariantMap estimate(const QString& sequenceId, const QString& presetId, const QVariantMap& overrides) const;
    /// plan() and estimate() in one: a gaming card's line.
    Q_INVOKABLE QVariantMap estimateFor(const QString& id, const QString& sequenceId, const QString& range) const;

    // ---- The arithmetic (tests) ----
    /// The video bitrate (bits/s) that fills 95 % of `targetBytes` over `seconds` with `audioBitrate` (bits/s) audio;
    /// 0 when nothing fits.
    static std::int64_t targetVideoBitrate(std::int64_t targetBytes, double seconds, std::int64_t audioBitrate);
    /// The encode time in seconds of `frames` frames of `width` x `height` with FFmpeg encoder `encoder` ("h264_nvenc",
    /// "libx264", ...) for `codec` ("h264" | "hevc" | "prores" | "dnxhr"): P.1's measured pipeline speeds, a margin.
    static double encodeSeconds(const std::string& codec, const std::string& encoder, int width, int height,
                                std::int64_t frames);
    /// "142 MB", "850 KB", "1.24 GB" (decimal units, as upload limits are given).
    static QString sizeText(double bytes);
    /// "about 45 s", "about 3 min 20 s", "about 1 h 12 min".
    static QString encodeText(double seconds);

private:
    ProjectController* controller() const;
    ExportController* exporter() const;
    QString encoderFor(const QString& codec, const std::string& configured) const;

    mutable QPointer<ProjectController> m_controller;
    mutable QPointer<ExportController> m_export;
};

struct GamingExportSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GamingExport)
    QML_NAMED_ELEMENT(GamingExport)
    QML_SINGLETON
public:
    static GamingExport* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return GamingExport::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
