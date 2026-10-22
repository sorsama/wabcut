pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: a track strip's sends (Premiere's Audio Track Mixer sends A to E): per send its submix, its level (drag
// the slider: one undo step per drag; double-click resets to 0 dB), Pre-Fader (P) and mute (M); a send's menu moves it
// to another submix or removes it. "Send" adds one to a submix or to a new one ("Create Stereo Submix"...).
ColumnLayout {
    id: root

    /// The track key ("t12").
    property string target: ""
    /// [{ index (1..5), submix, submixName, preFader, muted, levelDb, pan }]
    property var sends: []
    property string message: ""

    spacing: Theme.space(1) / 2

    // A small P / M switch of a send.
    component SendToggle: AbstractButton {
        id: toggle
        property color onColor: Theme.accentFill
        property string tip: ""
        implicitWidth: Theme.space(4)
        implicitHeight: Theme.space(4)
        checkable: true
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: tip
        background: Rectangle {
            radius: Theme.radiusBadge
            color: toggle.checked ? toggle.onColor : toggle.hovered ? Theme.hover : Theme.sunken
            border.width: Theme.hairline
            border.color: toggle.checked ? toggle.onColor : Theme.border
        }
        contentItem: Text {
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: toggle.text
            color: toggle.checked ? Theme.clipInk : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
        Loader {
            active: toggle.hovered && toggle.tip !== ""
            sourceComponent: Tooltip {
                parent: toggle
                visible: true
                text: toggle.tip
            }
        }
    }

    Repeater {
        model: root.sends
        delegate: ColumnLayout {
            id: send
            required property var modelData
            required property int index
            objectName: "sendSlot_" + index
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1) / 2
                AbstractButton {
                    id: sendName
                    Layout.fillWidth: true
                    implicitHeight: Theme.space(4)
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: qsTr("Send %1 to %2").arg(send.modelData.index).arg(send.modelData.submixName)
                    onClicked: sendMenu.popup(sendName, 0, sendName.height)
                    background: Rectangle {
                        radius: Theme.radiusBadge
                        color: sendName.hovered ? Theme.hover : "transparent"
                    }
                    contentItem: Text {
                        text: send.modelData.submixName
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        color: send.modelData.muted ? Theme.textTertiary : Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.strikeout: send.modelData.muted
                    }
                    OptionsMenu {
                        id: sendMenu
                        objectName: "sendMenu"
                        options: AudioMixer.outputs.filter(o => o.id !== "mix").concat([{ id: "remove", name: qsTr("Remove send") }])
                        current: send.modelData.submix
                        onChosen: id => root.message = id === "remove" ? AudioMixer.removeSend(root.target, send.modelData.index)
                                                                       : AudioMixer.setSendSubmix(root.target, send.modelData.index, id)
                    }
                }
                SendToggle {
                    objectName: "sendPreFader"
                    //: Pre-Fader (one letter on a mixer send)
                    text: qsTr("P")
                    tip: send.modelData.preFader ? qsTr("Pre-Fader: the send ignores the track's fader") : qsTr("Post-Fader: the send follows the track's fader")
                    checked: send.modelData.preFader
                    onToggled: root.message = AudioMixer.setSendOption(root.target, send.modelData.index, "preFader", checked)
                }
                SendToggle {
                    objectName: "sendMute"
                    //: Mute (one letter on a mixer send)
                    text: qsTr("M")
                    tip: qsTr("Mute this send")
                    onColor: Theme.warning
                    checked: send.modelData.muted
                    onToggled: root.message = AudioMixer.setSendOption(root.target, send.modelData.index, "muted", checked)
                }
            }
            ValueSlider {
                id: level
                objectName: "sendLevel"
                Layout.fillWidth: true
                implicitWidth: Theme.space(10)
                from: -60
                to: 12
                origin: 0
                stepSize: 0.1
                value: Math.max(-60, send.modelData.levelDb)
                Accessible.name: qsTr("Send %1 volume, %2 dB").arg(send.modelData.index).arg(Number(send.modelData.levelDb).toFixed(1))
                onMoved: root.message = AudioMixer.setSendLevel(root.target, send.modelData.index, value <= -60 ? -144 : value)
                onPressedChanged: if (!pressed) AudioMixer.endSendGesture()
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        root.message = AudioMixer.setSendLevel(root.target, send.modelData.index, 0)
                        AudioMixer.endSendGesture()
                    }
                }
            }
        }
    }

    // Add a send (at most five).
    AbstractButton {
        id: addSend
        objectName: "sendAdd"
        Layout.fillWidth: true
        visible: root.sends.length < 5
        implicitHeight: Theme.controlHeightSmall - Theme.space(1)
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: qsTr("Add a send")
        onClicked: addMenu.popup(addSend, 0, addSend.height)
        background: Rectangle {
            radius: Theme.radiusBadge
            color: addSend.hovered ? Theme.hover : "transparent"
            border.width: Theme.hairline
            border.color: Theme.border
        }
        contentItem: RowLayout {
            spacing: Theme.space(1)
            Icon {
                Layout.leftMargin: Theme.space(1)
                name: "plus"
                size: Theme.iconXs
                color: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Send")
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: addSend.visualFocus
        }
        OptionsMenu {
            id: addMenu
            objectName: "sendAddMenu"
            options: AudioMixer.outputs.filter(o => o.id !== "mix").concat(AudioMixer.submixFormats)
            onChosen: id => root.message = AudioMixer.addSend(root.target, id)
        }
    }
}
