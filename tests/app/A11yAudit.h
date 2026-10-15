#pragma once

// Accessibility audit for the UI tests (M5n): the interactive controls under an item that a screen reader cannot name,
// and the controls the keyboard reaches. Reusable over any panel or dialog:
//
//     A11yScope a11y;                                   // QAccessible on, Tab through every control (restored after)
//     const QStringList issues = auditAccessibility(panel); // empty when every control has a name and a fitting role
//     CHECK(tabReaches(panel, "reviewTabs"));           // Tab (QQuickItem::nextItemInFocusChain) gets there
//     CHECK(pressWithKey(window, item, Qt::Key_Space)); // focus it as Tab would, press the key
//
// What counts as interactive: buttons (QQuickAbstractButton: IconButton, PillButton, Toggle, Swatch, menu items...),
// editable text (TextField, TextArea, NumberField, FieldInput), ranges and choices (Slider, SpinBox, Dial, ComboBox),
// any other Control that takes Tab focus (PillTabs), and plain items with activeFocusOnTab (a focus scope is judged by
// the item that holds its focus). Hidden or zero-sized items are left out, disabled ones are not (a screen reader
// announces them as unavailable). Controls inside a text field, slider or combo box are its own parts and not visited.
// Mouse-only gestures (a TapHandler without a key or press action) cannot be seen from the item tree; review those by
// hand.

#include <QAccessible>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QList>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStyleHints>

namespace wabcut::test {

/// QAccessible on and Tab through every control (Windows' default; the offscreen platform's is text fields only), for
/// the lifetime of the scope. Create it before the shell so the controls register their accessible names.
class A11yScope {
public:
    A11yScope() : m_tab(QGuiApplication::styleHints()->tabFocusBehavior()) {
        QAccessible::setActive(true);
        QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
    }
    ~A11yScope() { QGuiApplication::styleHints()->setTabFocusBehavior(m_tab); }
    A11yScope(const A11yScope&) = delete;
    A11yScope& operator=(const A11yScope&) = delete;

private:
    Qt::TabFocusBehavior m_tab;
};

enum class A11yKind { None, Button, Text, Range, Focusable };

/// What a screen reader and the keyboard expect of `item`.
inline A11yKind a11yKind(const QQuickItem* item) {
    if (!item)
        return A11yKind::None;
    // Containers and scrolling chrome are not controls (a scroll bar is driven by the wheel and its view's keys).
    if (item->inherits("QQuickPane") || item->inherits("QQuickScrollBar") || item->inherits("QQuickScrollIndicator"))
        return A11yKind::None;
    if (item->inherits("QQuickAbstractButton"))
        return A11yKind::Button;
    if (item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit"))
        return item->property("readOnly").toBool() ? A11yKind::None : A11yKind::Text;
    if (item->inherits("QQuickSlider") || item->inherits("QQuickRangeSlider") || item->inherits("QQuickDial")
        || item->inherits("QQuickSpinBox") || item->inherits("QQuickComboBox") || item->inherits("QQuickTumbler"))
        return A11yKind::Range;
    if (item->inherits("QQuickControl"))
        return (item->property("focusPolicy").toInt() & Qt::TabFocus) ? A11yKind::Focusable : A11yKind::None;
    return item->activeFocusOnTab() ? A11yKind::Focusable : A11yKind::None;
}

/// "IconButton(reviewNoteTime_3)", for messages.
inline QString a11yLabel(const QQuickItem* item) {
    QString type = QString::fromLatin1(item->metaObject()->className());
    if (const qsizetype cut = type.indexOf(QLatin1String("_QMLTYPE")); cut > 0)
        type.truncate(cut);
    if (const qsizetype cut = type.indexOf(QLatin1String("_QML_")); cut > 0)
        type.truncate(cut);
    return item->objectName().isEmpty() ? type : type + QLatin1Char('(') + item->objectName() + QLatin1Char(')');
}

/// The control's place under `root`: the nearest named ancestors, then the control.
inline QString a11yPath(const QQuickItem* root, const QQuickItem* item) {
    QStringList parts{a11yLabel(item)};
    for (const QQuickItem* p = item->parentItem(); p && p != root && parts.size() < 4; p = p->parentItem()) {
        if (!p->objectName().isEmpty())
            parts.prepend(p->objectName());
    }
    return parts.join(QLatin1String(" > "));
}

/// The item a focus scope gives its focus to (the scope itself when it holds none).
inline const QQuickItem* a11yFocusTarget(const QQuickItem* item) {
    const QQuickItem* target = item;
    for (int depth = 0; depth < 16 && target->isFocusScope(); ++depth) {
        const QQuickItem* inner = target->scopedFocusItem();
        if (!inner || inner == target)
            break;
        target = inner;
    }
    return target;
}

inline bool a11yRoleFits(A11yKind kind, QAccessible::Role role) {
    switch (kind) {
    case A11yKind::Button:
        return role != QAccessible::NoRole && role != QAccessible::Client && role != QAccessible::Pane
            && role != QAccessible::Grouping && role != QAccessible::StaticText && role != QAccessible::Graphic
            && role != QAccessible::Separator && role != QAccessible::Window && role != QAccessible::Border
            && role != QAccessible::Whitespace && role != QAccessible::LayeredPane;
    case A11yKind::Text:
        return role == QAccessible::EditableText || role == QAccessible::SpinBox || role == QAccessible::ComboBox;
    case A11yKind::Range:
        return role == QAccessible::Slider || role == QAccessible::SpinBox || role == QAccessible::Dial
            || role == QAccessible::ComboBox || role == QAccessible::ProgressBar;
    case A11yKind::Focusable:
        return role != QAccessible::NoRole && role != QAccessible::Client;
    case A11yKind::None:
        break;
    }
    return true;
}

/// The interactive controls under `root` (visible ones) without an accessible name or with a role that does not fit
/// ("Panel > IconButton(x): no accessible name"). `skip`: objectNames whose subtrees are left out.
inline QStringList auditAccessibility(QQuickItem* root, const QStringList& skip = {}) {
    QStringList issues;
    if (!root)
        return {QStringLiteral("no item to audit")};
    QList<QQuickItem*> stack{root};
    while (!stack.isEmpty()) {
        QQuickItem* item = stack.takeLast();
        if (!item->isVisible() || item->width() <= 0 || item->height() <= 0 || skip.contains(item->objectName()))
            continue;
        const A11yKind kind = item == root ? A11yKind::None : a11yKind(item);
        if (kind != A11yKind::None) {
            const QQuickItem* target = kind == A11yKind::Focusable ? a11yFocusTarget(item) : item;
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(const_cast<QQuickItem*>(target));
            if (!iface) {
                issues << a11yPath(root, item) + QStringLiteral(": not in the accessibility tree");
            } else {
                if (iface->text(QAccessible::Name).trimmed().isEmpty())
                    issues << a11yPath(root, item) + QStringLiteral(": no accessible name");
                if (!a11yRoleFits(kind, iface->role()))
                    issues << a11yPath(root, item)
                                  + QStringLiteral(": role 0x%1 does not fit").arg(static_cast<int>(iface->role()), 0, 16);
            }
        }
        // A text field's, slider's or combo box's own parts are not controls of their own.
        if (kind == A11yKind::Text || kind == A11yKind::Range)
            continue;
        const QList<QQuickItem*> children = item->childItems();
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stack << *it;
    }
    return issues;
}

/// The items Tab visits inside `root`, in order (QQuickItem::nextItemInFocusChain from `root`; visible and enabled
/// ones, as Tab finds them). Needs A11yScope's Tab behaviour.
inline QList<QQuickItem*> tabChain(QQuickItem* root, int limit = 600) {
    QList<QQuickItem*> chain;
    if (!root || !root->window())
        return chain;
    QSet<QQuickItem*> seen;
    bool inside = false;
    QQuickItem* item = root;
    for (int i = 0; i < limit; ++i) {
        item = item->nextItemInFocusChain(true);
        if (!item || seen.contains(item))
            break;
        seen.insert(item);
        if (item == root || root->isAncestorOf(item)) {
            chain << item;
            inside = true;
        } else if (inside) {
            break; // past the end of the subtree
        }
    }
    return chain;
}

/// Tab reaches the item named `objectName` inside `root`.
inline bool tabReaches(QQuickItem* root, const QString& objectName) {
    for (const QQuickItem* item : tabChain(root)) {
        if (item->objectName() == objectName)
            return true;
    }
    return false;
}

/// The chain's objectNames (unnamed items as their types), for messages.
inline QString describeTabChain(QQuickItem* root) {
    QStringList names;
    for (const QQuickItem* item : tabChain(root))
        names << a11yLabel(item);
    return names.join(QLatin1String(" > "));
}

/// Focuses `item` as Tab would and presses `key` in its window; false when it could not take the focus.
inline bool pressWithKey(QQuickWindow* window, QQuickItem* item, int key, const QString& text = {}) {
    if (!window || !item)
        return false;
    item->forceActiveFocus(Qt::TabFocusReason);
    if (!item->hasActiveFocus())
        return false;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &release);
    return true;
}

} // namespace wabcut::test
