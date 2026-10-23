pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A title of the menu bar (AppMenuBar, design language v3): a text pill, on `hover` under the pointer and `raised`
// while its menu is open. It takes the focus from Tab only (a click leaves the focus where it was, so the menu's command
// still acts on the panel you were in); Space, Enter or Down open its menu with the first row highlighted, Left and Right
// move to the neighbouring title.
AbstractButton {
    id: control

    /// Its menu is open.
    property bool open: false
    /// An icon after the label (the collapsed bar's Menu button).
    property string iconName: ""
    /// Space / Enter / Down (keyboard) want the menu.
    signal keyboardOpenRequested()
    /// Left (-1) / Right (+1): the focus to the neighbouring title.
    signal stepRequested(int delta)

    implicitHeight: Theme.compact ? Theme.controlHeightSmall : Theme.controlHeight
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    padding: 0
    leftPadding: Theme.space(2)
    rightPadding: Theme.space(control.iconName !== "" ? 1.5 : 2)
    spacing: Theme.space(1)
    focusPolicy: Qt.TabFocus
    hoverEnabled: true

    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd
    font.weight: Theme.weightLabel

    Accessible.role: Accessible.ButtonMenu
    Accessible.name: control.text

    // Space comes as a click (AbstractButton); Enter, Return and Down open the menu too. Left / Right are claimed before
    // the keymap (they step the playhead otherwise).
    Keys.onShortcutOverride: event => {
        event.accepted = event.modifiers === Qt.NoModifier
                         && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Down, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
    }
    Keys.onPressed: event => {
        if (event.modifiers !== Qt.NoModifier)
            return
        if (event.key === Qt.Key_Down || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            control.keyboardOpenRequested()
            event.accepted = true
        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            control.stepRequested(event.key === Qt.Key_Left ? -1 : 1)
            event.accepted = true
        }
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.font
                color: control.open || control.hovered || control.visualFocus ? Theme.textPrimary : Theme.textSecondary
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName !== ""
                name: control.iconName
                size: Theme.iconSm
                color: control.open || control.hovered ? Theme.textPrimary : Theme.textSecondary
            }
        }
    }

    background: Rectangle {
        radius: height / 2
        color: control.open ? Theme.raised : control.down || control.hovered ? Theme.hover : "transparent"

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: parent.radius
            visible: control.visualFocus
        }
    }
}
