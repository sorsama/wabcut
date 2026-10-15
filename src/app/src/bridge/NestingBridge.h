#pragma once

// NestingBridge (M5l, QML singleton `Nesting`): nested sequences in the timeline.
//
//   * Nest… (the clip menu, Premiere's right-click > Nest…): sequence.nest on the selected clips (and their linked
//     partners), named in a small dialog ("Nested Sequence 01" by default); one undo step.
//   * A double-click on a nested or multi-camera clip opens its sequence in a tab (SequenceTabs, M5i) with the playhead
//     on the frame the clip showed. The breadcrumb leads back to the sequences it was opened from.
//   * Unnest (sequence.unnest): refused with a reason that is shown as it is.
//   * Reveal nested sequence in Library (Library.showItem).
//
// Principle 9: QML sees keys ("c12", "s3") and names.

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <unordered_map>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class LibraryBridge;
class ProjectController;
class SequenceTabsBridge;

class NestingBridge : public QObject {
    Q_OBJECT
    /// The way back to the sequence the current one was opened from: [{ key ("s3"), name, current }], outermost first;
    /// empty when the current sequence was not opened from a nested clip.
    Q_PROPERTY(QVariantList breadcrumb READ breadcrumb NOTIFY breadcrumbChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit NestingBridge(QObject* parent = nullptr);
    static NestingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setTabs(SequenceTabsBridge* tabs);
    void setLibrary(LibraryBridge* library);

    QVariantList breadcrumb() const;
    QString message() const { return m_message; }

    /// The name Nest… proposes: "Nested Sequence 01", 02, … (the first one not taken).
    Q_INVOKABLE QString defaultName() const;
    /// Clips are selected in the current sequence.
    Q_INVOKABLE bool canNest() const;
    /// Nest the selected clips into a new sequence called `name`. Returns "" or why not.
    Q_INVOKABLE QString nestSelection(const QString& name);
    /// What a clip is: { nested, multicam, sequence ("s3"), name (the nested sequence's), angle (the multi-camera clip's
    /// angle name), angleNumber }; an empty map for a clip that is not in the current sequence.
    Q_INVOKABLE QVariantMap clipInfo(const QString& clipKey) const;
    /// Opens a nested or multi-camera clip's sequence in a tab, the playhead on the frame it shows.
    Q_INVOKABLE bool openNested(const QString& clipKey);
    /// Goes back to a sequence of the breadcrumb.
    Q_INVOKABLE bool goTo(const QString& sequenceKey);
    /// Unnest a nested clip. Returns "" or the reason it cannot.
    Q_INVOKABLE QString unnest(const QString& clipKey);
    /// Shows the clip's nested sequence in the Library.
    Q_INVOKABLE bool revealInLibrary(const QString& clipKey);

signals:
    void breadcrumbChanged();
    void messageChanged();
    /// QML: the Library should come forward (Main shows the browser panel).
    void libraryRequested();

private:
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    QPointer<SequenceTabsBridge> m_tabs;
    QPointer<LibraryBridge> m_library;
    /// Nested sequence -> the sequence it was opened from (the breadcrumb's links).
    std::unordered_map<std::uint64_t, SequenceId> m_openedFrom;
    QString m_message;
};

struct NestingBridgeForeign {
    Q_GADGET
    QML_FOREIGN(NestingBridge)
    QML_NAMED_ELEMENT(Nesting)
    QML_SINGLETON
public:
    static NestingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return NestingBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
