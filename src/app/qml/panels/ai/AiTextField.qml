import QtQuick
import QtQuick.Controls.Basic

// Text field of the AI preferences (keys, base URLs, model ids): a sunken pill with a hairline edge (r07: it sits on
// raised cards, where a raised fill made it invisible) that becomes the focus ring's colour while focused. `masked` hides what is typed (API keys); a masked field never shows a stored key,
// only what the user is pasting right now.
TextField {
    id: control

    property bool masked: false
    property bool monospace: false

    implicitHeight: Theme.controlHeightSmall
    leftPadding: Theme.space(3)
    rightPadding: Theme.space(3)
    verticalAlignment: TextInput.AlignVCenter
    echoMode: masked ? TextInput.Password : TextInput.Normal
    passwordCharacter: "•"
    color: Theme.textPrimary
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.accentFill
    selectedTextColor: Theme.accentOnFill
    font.family: monospace ? Theme.fontMono : Theme.fontSans
    font.pixelSize: Theme.textMd
    inputMethodHints: masked ? (Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText) : Qt.ImhNone
    selectByMouse: true

    Accessible.name: placeholderText

    // Editor shortcuts must not fire while typing here.
    Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                                                       || event.key === Qt.Key_Delete || event.key === Qt.Key_Left
                                                       || event.key === Qt.Key_Right || event.key === Qt.Key_Home
                                                       || event.key === Qt.Key_End

    background: Rectangle {
        radius: height / 2
        color: Theme.sunken
        border.width: control.activeFocus ? Theme.focusRingWidth : Theme.hairline
        border.color: control.activeFocus ? Theme.focus : Theme.borderStrong
        opacity: control.enabled ? 1 : Theme.disabledOpacity
    }
}
