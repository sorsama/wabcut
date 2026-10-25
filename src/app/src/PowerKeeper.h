#pragma once

// PowerKeeper (0.4): keeps the computer from sleeping on its idle timer while `active` (Main binds it to the setting
// and the background work). Windows: one process-wide power request (PowerCreateRequest with the reason "Wabcut is
// exporting or processing media", PowerSetRequest/PowerClearRequest(PowerRequestSystemRequired)) set while any keeper is
// active; the display may still turn off. Elsewhere it does nothing. `powercfg /requests` names Wabcut while it holds.
//
// QML: `PowerKeeper { active: Preferences.keepAwake && window.keepAwakeNeeded }`. GUI thread only.

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace wabcut::app {

class PowerKeeper : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// While true this keeper holds the computer awake (with every other active keeper: one request for all).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)

public:
    explicit PowerKeeper(QObject* parent = nullptr);
    ~PowerKeeper() override; ///< lets go of its hold

    bool active() const { return m_active; }
    void setActive(bool on);

    /// Keepers active now (all instances).
    static int holds();
    /// Tests: replaces the platform call; gets true when the first keeper becomes active, false when the last lets go.
    /// An empty function restores the platform's.
    static void setBackendForTests(std::function<void(bool hold)> backend);

signals:
    void activeChanged();

private:
    bool m_active = false;
};

} // namespace wabcut::app
