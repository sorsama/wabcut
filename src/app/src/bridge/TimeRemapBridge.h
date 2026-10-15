#pragma once

#include <wabcut/command/Command.h>
#include <wabcut/model/Clip.h>

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace wabcut::app {
class ProjectController;

// UI units only: normalized clip positions, seconds and percentages. Every mutation runs a C.6 command.
class TimeRemapBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed FINAL)
    Q_PROPERTY(QString clipName READ clipName NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList keys READ keys NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList sourcePoints READ sourcePoints NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList speedPoints READ speedPoints NOTIFY changed FINAL)
    Q_PROPERTY(double sourceMin READ sourceMin NOTIFY changed FINAL)
    Q_PROPERTY(double sourceMax READ sourceMax NOTIFY changed FINAL)
    Q_PROPERTY(double duration READ duration NOTIFY changed FINAL)
    Q_PROPERTY(double playhead READ playhead NOTIFY changed FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
public:
    explicit TimeRemapBridge(QObject* parent = nullptr);
    ~TimeRemapBridge() override;
    static TimeRemapBridge* create(QQmlEngine*, QJSEngine*);
    void setController(ProjectController* controller);
    bool available() const;
    bool enabled() const;
    QString clipName() const;
    QVariantList keys() const;
    QVariantList sourcePoints() const;
    QVariantList speedPoints() const;
    double sourceMin() const;
    double sourceMax() const;
    double duration() const;
    double playhead() const;
    QString message() const { return m_message; }

    Q_INVOKABLE bool setEnabled(bool enabled);
    Q_INVOKABLE bool addKey(double fraction);
    Q_INVOKABLE bool moveKey(int index, double fraction);
    Q_INVOKABLE bool setSpeed(int segment, double percent);
    Q_INVOKABLE bool setInterpolation(int index, const QString& interpolation);
    Q_INVOKABLE bool removeKey(int index);
    Q_INVOKABLE bool splitKey(int index, double fraction, bool left);
    Q_INVOKABLE bool hold(double fraction, double seconds);
    Q_INVOKABLE bool reverse(double fraction, double seconds);
    Q_INVOKABLE bool preset(const QString& name);
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void cancelGesture();
    static double speedAt(const Clip& clip, Ticks curveTime);
    static QString speedLabel(const Clip& clip);
    static double speedFraction(double percent);
    static double fractionSpeed(double fraction);
signals:
    void changed();
    void messageChanged();

private:
    std::optional<Clip> clip() const;
    Ticks timelineAt(const Clip& clip, double fraction) const;
    bool run(const char* commandName, command::Json params);
    bool batch(std::vector<command::CommandRequest> commands);
    bool insertSegment(const char* commandName, double fraction, double seconds);
    void setMessage(const QString& text);
    QPointer<ProjectController> m_controller;
    ClipId m_gestureClip;
    bool m_gesture = false;
    QString m_message;
};
struct TimeRemapSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::TimeRemapBridge)
    QML_NAMED_ELEMENT(TimeRemap)
    QML_SINGLETON
public:
    static TimeRemapBridge* create(QQmlEngine* engine, QJSEngine* script) {
        return TimeRemapBridge::create(engine, script);
    }
};
} // namespace wabcut::app
