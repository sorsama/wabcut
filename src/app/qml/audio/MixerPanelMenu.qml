import QtQuick

// The Audio Track Mixer's panel menu (0.7, card 032; Premiere's panel menu): Show/Hide Tracks…, Meter Input(s) Only and
// Loop, each with its key (audioMixer.showHideTracks Ctrl+Alt+T, audioMixer.meterInputsOnly Ctrl+Shift+I, audioMixer.loop
// Ctrl+L). Show/Hide Tracks opens its checklist once the menu has closed (`showHideRequested`).
// (Its own file: QtQuick.Controls, which the panel imports, has a ContextMenu type of its own.)
ContextMenu {
    id: root
    objectName: "mixerPanelMenu"

    signal showHideRequested()
    property bool showHidePending: false

    ContextMenuItem {
        objectName: "mixerMenuShowHide"
        text: qsTr("Show/Hide Tracks…")
        iconName: "eye"
        command: "audioMixer.showHideTracks"
        hint: qsTr("Choose which strips the mixer shows. A hidden track still plays")
        onTriggered: root.showHidePending = true
    }
    ContextMenuItem {
        id: inputsItem
        objectName: "mixerMenuMeterInputs"
        text: qsTr("Meter Inputs Only")
        command: "audioMixer.meterInputsOnly"
        checkable: true
        checked: AudioMixer.meterInputsOnly
        hint: qsTr("While the microphone is open, a record-armed track's meter shows the input and the others stay empty")
        onTriggered: {
            AudioMixer.meterInputsOnly = !AudioMixer.meterInputsOnly
            inputsItem.checked = Qt.binding(() => AudioMixer.meterInputsOnly) // the toggle broke it
        }
    }
    ContextMenuItem {
        id: loopItem
        objectName: "mixerMenuLoop"
        text: qsTr("Loop")
        command: "audioMixer.loop"
        checkable: true
        checked: Playback.loop
        hint: qsTr("Playback repeats the sequence, or its In to Out range")
        onTriggered: {
            Playback.loop = !Playback.loop
            loopItem.checked = Qt.binding(() => Playback.loop)
        }
    }

    onClosed: {
        if (root.showHidePending) {
            root.showHidePending = false
            root.showHideRequested()
        }
    }
}
