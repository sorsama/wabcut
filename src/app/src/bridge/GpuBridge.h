#pragma once

// Gpu (singleton `Gpu`; issue #10, docs/performance.md "Which GPU Wabcut uses"): Settings > Playback & performance >
// Graphics card. Automatic is the high-performance GPU (discrete before integrated), unless Windows Graphics settings
// name a GPU for wabcut.exe; or one of this computer's GPUs (the integrated one saves battery). The choice is kept in the
// settings ("graphics/adapter": the adapter's stable key), reaches the export encoders at once (media::setGpuChoice) and
// the preview, the offscreen renderer and the hardware decoders at the next start (main() applies it before any window
// exists: applySavedChoice(), then RhiPolicy's applyPreviewAdapter()).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class GpuBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Gpu)
    QML_SINGLETON

    /// This computer's GPUs, high performance first: [{ key, name, vendor, kind ("integrated", "discrete", ""),
    /// memory (bytes of video memory), detail ("Integrated · saves battery", "Discrete · 8 GB"), inUse }].
    Q_PROPERTY(QVariantList adapters READ adapters NOTIFY adaptersChanged FINAL)
    /// "" = Automatic, else an adapter key.
    Q_PROPERTY(QString choice READ choice WRITE setChoice NOTIFY choiceChanged FINAL)
    /// The name of the GPU the choice picks now (the export encoders' GPU).
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY choiceChanged FINAL)
    /// The menu's first entry: "Automatic (NVIDIA GeForce RTX 5060 Laptop GPU)".
    Q_PROPERTY(QString automaticText READ automaticText NOTIFY adaptersChanged FINAL)
    /// The GPU the preview and decoding use in this run, and why ("the high-performance GPU, chosen automatically").
    Q_PROPERTY(QString inUseName READ inUseName NOTIFY adaptersChanged FINAL)
    Q_PROPERTY(QString inUseReason READ inUseReason NOTIFY adaptersChanged FINAL)
    /// The choice picks another GPU than the preview's: it uses that one after a restart.
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY choiceChanged FINAL)
    /// The preview follows the choice (Windows); elsewhere the system picks the preview's GPU and the choice only
    /// orders the export encoders.
    Q_PROPERTY(bool previewFollows READ previewFollows CONSTANT FINAL)

public:
    explicit GpuBridge(QObject* parent = nullptr);

    /// main(), before anything asks which GPU to use: the saved choice to media::setGpuChoice().
    static void applySavedChoice();
    /// The reason in words for a media::gpuReasonKey() ("highPerformance" -> "the high-performance GPU, chosen
    /// automatically").
    static QString reasonText(const QString& reasonKey);

    QVariantList adapters() const;
    QString choice() const;
    void setChoice(const QString& key);
    QString selectedName() const;
    QString automaticText() const;
    QString inUseName() const;
    QString inUseReason() const;
    bool restartNeeded() const;
    bool previewFollows() const;

    /// Reads the adapters again (tests that replaced them).
    Q_INVOKABLE void refresh();
    /// Windows Settings > System > Display > Graphics, where a GPU can be set for wabcut.exe.
    Q_INVOKABLE void openWindowsGraphicsSettings();

signals:
    void adaptersChanged();
    void choiceChanged();
};

} // namespace wabcut::app
