#pragma once

// KeyboardTrainerBridge: the Keyboard Trainer mini-game (M5j onboarding), for QML (singleton `Trainer`).
//
// Wabcut names an editing action ("Ripple Delete", "Add Edit", "Mark In") and the player presses its shortcut. The
// answer is resolved against the active keymap (KeymapDispatcher's: Premiere's defaults plus the user's overrides or
// imported .kys), so a custom key set trains its own keys. A round is ten prompts from one category (editing, copy and
// paste, marks and markers, trim, playback, tools, timeline, multi-camera) or from all; commands without a key in
// the active keymap are left out.
//   * First-try answers count toward accuracy; a wrong key keeps the prompt (try again, or show the hint).
//   * Score: 100 a prompt, up to 50 more for speed (under two seconds), 10 more per streak step; half with the hint.
//   * The best streak is kept by Preferences (trainerBest).
//
// Principle 9: QML sees command display names, key text and numbers.

#include <wabcut/keymap/KeymapManager.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <random>
#include <string>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class KeymapDispatcher;

class KeyboardTrainerBridge : public QObject {
    Q_OBJECT
    /// [{ key, label, count }] — count: prompts with a key in the active keymap.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY keymapChanged FINAL)
    Q_PROPERTY(QString category READ category NOTIFY stateChanged FINAL)
    /// "idle", "asking", "correct", "wrong" or "finished".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    /// The action to perform ("Ripple Delete") and its menu or panel ("Edit").
    Q_PROPERTY(QString prompt READ prompt NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString promptGroup READ promptGroup NOTIFY stateChanged FINAL)
    /// The prompt's command id ("edit.rippleDelete").
    Q_PROPERTY(QString promptCommand READ promptCommand NOTIFY stateChanged FINAL)
    /// The expected shortcut, as the menus show it ("Shift+Delete", "⇧⌫"); shown after showHint().
    Q_PROPERTY(QString hint READ hint NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool hintShown READ hintShown NOTIFY stateChanged FINAL)
    /// The last key pressed and what it does in the active keymap ("" when nothing).
    Q_PROPERTY(QString lastPressed READ lastPressed NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString lastPressedAction READ lastPressedAction NOTIFY stateChanged FINAL)
    Q_PROPERTY(int round READ round NOTIFY stateChanged FINAL)          ///< 1-based prompt number
    Q_PROPERTY(int roundLength READ roundLength NOTIFY stateChanged FINAL)
    Q_PROPERTY(int correct READ correct NOTIFY stateChanged FINAL)      ///< first-try answers
    Q_PROPERTY(int streak READ streak NOTIFY stateChanged FINAL)
    Q_PROPERTY(int bestStreak READ bestStreak NOTIFY stateChanged FINAL)
    Q_PROPERTY(int score READ score NOTIFY stateChanged FINAL)
    Q_PROPERTY(int lastPoints READ lastPoints NOTIFY stateChanged FINAL) ///< what the last correct answer scored
    Q_PROPERTY(int lastMs READ lastMs NOTIFY stateChanged FINAL)        ///< time to the last correct answer
    Q_PROPERTY(int averageMs READ averageMs NOTIFY stateChanged FINAL)

public:
    struct Prompt {
        std::string command;
        const char* category;
    };

    explicit KeyboardTrainerBridge(QObject* parent = nullptr);
    static KeyboardTrainerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setDispatcher(KeymapDispatcher* dispatcher);
    /// Tests: train against this keymap instead of the dispatcher's.
    void setKeymap(const keymap::KeymapManager* keymap);
    /// Tests: a fixed prompt order.
    void setSeed(unsigned seed) { m_random.seed(seed); }

    /// Every prompt the trainer knows (command id and category), in order.
    static const std::vector<Prompt>& allPrompts();

    QVariantList categories() const;
    QString category() const { return m_category; }
    QString state() const { return m_state; }
    QString prompt() const;
    QString promptGroup() const;
    QString promptCommand() const;
    QString hint() const;
    bool hintShown() const { return m_hintShown; }
    QString lastPressed() const { return m_lastPressed; }
    QString lastPressedAction() const { return m_lastPressedAction; }
    int round() const { return m_index + 1; }
    int roundLength() const { return static_cast<int>(m_queue.size()); }
    int correct() const { return m_correct; }
    int streak() const { return m_streak; }
    int bestStreak() const { return m_best; }
    int score() const { return m_score; }
    int lastPoints() const { return m_lastPoints; }
    int lastMs() const { return m_lastMs; }
    int averageMs() const { return m_answered > 0 ? static_cast<int>(m_totalMs / m_answered) : 0; }

    /// Starts a round of ten from "all" or a category key; false when it has no prompts with keys.
    Q_INVOKABLE bool start(const QString& category);
    /// A key press (QKeyEvent key and modifiers): "correct", "wrong" or "ignored" (a bare modifier, or no round).
    Q_INVOKABLE QString press(int key, int modifiers);
    Q_INVOKABLE void showHint();
    /// Skips the prompt (a miss), or moves on after a correct answer.
    Q_INVOKABLE void next();
    Q_INVOKABLE void stop();
    /// The best streak saved earlier (Preferences.trainerBest).
    Q_INVOKABLE void setSavedBest(int best);

signals:
    void stateChanged();
    void keymapChanged();
    /// A correct answer (QML plays the spark).
    void answeredCorrectly();

private:
    const keymap::KeymapManager* keymap() const;
    const keymap::CommandInfo* currentInfo() const;
    QString chordText(const keymap::KeyChord& chord) const;
    void setState(const QString& state);

    QPointer<KeymapDispatcher> m_dispatcher;
    const keymap::KeymapManager* m_keymap = nullptr;
    std::mt19937 m_random{std::random_device{}()};
    QString m_category = QStringLiteral("all");
    QString m_state = QStringLiteral("idle");
    std::vector<std::string> m_queue;
    int m_index = 0;
    bool m_hintShown = false;
    bool m_missed = false; ///< a wrong key or the hint on this prompt
    QString m_lastPressed;
    QString m_lastPressedAction;
    int m_correct = 0;
    int m_streak = 0;
    int m_best = 0;
    int m_score = 0;
    int m_lastPoints = 0;
    int m_lastMs = 0;
    qint64 m_totalMs = 0;
    int m_answered = 0;
    QElapsedTimer m_clock;
};

struct KeyboardTrainerSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::KeyboardTrainerBridge)
    QML_NAMED_ELEMENT(Trainer)
    QML_SINGLETON
public:
    static KeyboardTrainerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return KeyboardTrainerBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
