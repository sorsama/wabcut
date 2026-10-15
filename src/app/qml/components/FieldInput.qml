import QtQuick
import QtQuick.Controls.Basic

// A single-line text field (design language v3): a raised pill with the focus ring's colour as its edge while it has
// focus. Set `iconName` for a leading icon (search fields). Esc clears it when `clearOnEscape` is set.
TextField {
    id: control

    property string iconName: ""
    property bool clearOnEscape: false

    implicitHeight: Theme.controlHeightSmall + Theme.space(1)
    leftPadding: iconName !== "" ? Theme.space(8.5) : Theme.space(3.5)
    rightPadding: Theme.space(3.5)
    placeholderTextColor: Theme.textTertiary
    color: Theme.textPrimary
    selectionColor: Theme.accentFill
    selectedTextColor: Theme.accentOnFill
    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd
    selectByMouse: true
    Keys.onEscapePressed: event => {
        if (control.clearOnEscape && control.text !== "") {
            control.text = ""
            event.accepted = true
        } else {
            event.accepted = false
        }
    }

    background: Rectangle {
        radius: height / 2
        color: control.activeFocus ? Theme.raised : control.hovered ? Theme.hover : Theme.raised
        border.width: control.activeFocus ? Theme.focusRingWidth : 0
        border.color: Theme.focus

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Icon {
        visible: control.iconName !== ""
        anchors.left: parent.left
        anchors.leftMargin: Theme.space(3.5)

        anchors.verticalCenter: parent.verticalCenter
        name: control.iconName
        size: Theme.iconSm
        color: Theme.textTertiary
    }
}
