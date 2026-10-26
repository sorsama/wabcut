#include "GpuBridge.h"

#include <wabcut/media/GpuInfo.h>

#include <QDesktopServices>
#include <QLocale>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>

#include <cmath>

namespace wabcut::app {

namespace {

const QString kChoiceKey = QStringLiteral("graphics/adapter");

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

/// "8", "0.5", "12": whole gigabytes without decimals.
QString gigabytes(std::uint64_t bytes) {
    const double gb = std::round(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0;
    return QLocale().toString(gb, 'f', gb >= 10.0 || gb == std::floor(gb) ? 0 : 1);
}

} // namespace

GpuBridge::GpuBridge(QObject* parent) : QObject(parent) {}

void GpuBridge::applySavedChoice() {
    media::setGpuChoice(QSettings().value(kChoiceKey).toString().toStdString());
}

QString GpuBridge::reasonText(const QString& reasonKey) {
    const auto reason = media::gpuReasonFromKey(reasonKey.toStdString());
    switch (reason.value_or(media::GpuReason::NoGpu)) {
    case media::GpuReason::NoGpu:
        return tr("no GPU found");
    case media::GpuReason::OnlyGpu:
        return tr("the only GPU");
    case media::GpuReason::HighPerformance:
        return tr("the high-performance GPU, chosen automatically");
    case media::GpuReason::WindowsPowerSaving:
        return tr("Power saving in Windows Graphics settings");
    case media::GpuReason::WindowsHighPerformance:
        return tr("High performance in Windows Graphics settings");
    case media::GpuReason::WindowsSpecific:
        return tr("the GPU Windows Graphics settings choose for Wabcut");
    case media::GpuReason::UserChoice:
        return tr("chosen in Settings");
    case media::GpuReason::UserChoiceMissing:
        return tr("the card chosen in Settings is not here, so Automatic");
    case media::GpuReason::Override:
        return tr("set by QT_D3D_ADAPTER_INDEX");
    }
    return {};
}

QVariantList GpuBridge::adapters() const {
    QVariantList list;
    const media::GpuSelection preview = media::previewGpu();
    for (const media::GpuAdapter& a : media::gpuAdapters()) {
        const QString kind = a.kind == media::GpuKind::Integrated ? QStringLiteral("integrated")
                             : a.kind == media::GpuKind::Discrete ? QStringLiteral("discrete")
                                                                  : QString();
        QString detail;
        if (a.kind == media::GpuKind::Integrated) {
            detail = tr("Integrated · saves battery");
        } else if (a.kind == media::GpuKind::Discrete) {
            //: %1 is gigabytes of video memory
            detail = a.dedicatedMemory > 0 ? tr("Discrete · %1 GB").arg(gigabytes(a.dedicatedMemory)) : tr("Discrete");
        } else if (a.dedicatedMemory > 0) {
            detail = tr("%1 GB video memory").arg(gigabytes(a.dedicatedMemory));
        }
        list.push_back(QVariantMap{{QStringLiteral("key"), qs(a.key)},
                                   {QStringLiteral("name"), qs(a.name)},
                                   {QStringLiteral("vendor"), qs(a.vendor)},
                                   {QStringLiteral("kind"), kind},
                                   {QStringLiteral("memory"), static_cast<double>(a.dedicatedMemory)},
                                   {QStringLiteral("detail"), detail},
                                   {QStringLiteral("inUse"), preview.adapter && preview.adapter->key == a.key}});
    }
    return list;
}

QString GpuBridge::choice() const {
    return qs(media::gpuChoice());
}

void GpuBridge::setChoice(const QString& key) {
    if (key == choice()) {
        return;
    }
    media::setGpuChoice(key.toStdString());
    QSettings settings;
    if (key.isEmpty()) {
        settings.remove(kChoiceKey);
    } else {
        settings.setValue(kChoiceKey, key);
    }
    emit choiceChanged();
}

QString GpuBridge::selectedName() const {
    const media::GpuSelection s = media::selectedGpu();
    return s.adapter ? qs(s.adapter->name) : QString();
}

QString GpuBridge::automaticText() const {
    media::GpuSelectionInput input;
    input.windows = media::windowsGpuSetting();
    const media::GpuSelection automatic = media::selectGpu(media::gpuAdapters(), input);
    //: Settings > Graphics card: %1 is the GPU Automatic picks
    return automatic.adapter ? tr("Automatic (%1)").arg(qs(automatic.adapter->name)) : tr("Automatic");
}

QString GpuBridge::inUseName() const {
    const media::GpuSelection s = media::previewGpu();
    return s.adapter ? qs(s.adapter->name) : QString();
}

QString GpuBridge::inUseReason() const {
    return reasonText(qs(std::string(media::gpuReasonKey(media::previewGpu().reason))));
}

bool GpuBridge::restartNeeded() const {
    const media::GpuSelection now = media::selectedGpu();
    const media::GpuSelection preview = media::previewGpu();
    return previewFollows() && now.adapter && preview.adapter && now.adapter->key != preview.adapter->key;
}

bool GpuBridge::previewFollows() const {
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

void GpuBridge::refresh() {
    emit adaptersChanged();
    emit choiceChanged();
}

void GpuBridge::openWindowsGraphicsSettings() {
#if defined(Q_OS_WIN)
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:display-advancedgraphics")));
#endif
}

} // namespace wabcut::app
