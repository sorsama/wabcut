pragma ComponentBehavior: Bound
import QtQuick

// A strip's automation mode (M5l; Premiere's Audio Track Mixer): Read follows the track's automation; Touch writes
// while the fader or pan is held and returns to the automation on release; Latch writes from the first touch until
// playback stops; Write writes from the start of playback. The dot turns red while this pass records the strip.
PillButton {
    id: root

    required property string target   // "t12" or "master"
    /// The strip's mode as AudioMixer.strips / master report it ("read", "touch", "latch", "write").
    property string mode: "read"
    readonly property bool writing: (AudioMixer.faderValues[target] ?? {}).writing === true

    objectName: "automationMode_" + target
    compact: true
    iconName: "chevron-down"
    text: mode === "touch" ? qsTr("Touch") : mode === "latch" ? qsTr("Latch") : mode === "write" ? qsTr("Write") : qsTr("Read")
    tooltip: qsTr("Automation mode")
    onClicked: modeMenu.popup(root, 0, root.height + Theme.space(1))

    // Recording this strip now.
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(0.5)
        width: Theme.dotSize
        height: Theme.dotSize
        radius: Theme.dotSize / 2
        visible: root.writing || root.mode !== "read"
        color: root.writing ? Theme.danger : Theme.warning
    }

    OptionsMenu {
        id: modeMenu
        options: AudioMixer.automationModes.map(m => ({ id: m.id, name: m.name }))
        current: root.mode
        onChosen: id => AudioMixer.setAutomationMode(root.target, id)
    }
}
