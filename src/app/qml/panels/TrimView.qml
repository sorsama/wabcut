pragma ComponentBehavior: Bound
import QtQuick

// The Trim view (M5i; Premiere's trim monitor, in the Program Monitor's well while Trim Edit is open): the outgoing
// frame | the incoming frame, each with its clip, source timecode and the frames trimmed since trim mode started, then
// trim −5 / −1, play around the edit, +1 / +5, the trim type and exit. Toggle Trim Type (Ctrl+Shift+T) cycles the
// type, J/K/L trims dynamically, Ctrl/⌥+arrows trim from the keyboard.
// 0.7 (card 127): the Slip type (Avid's slip): both edges of a clip are the edit points and the buttons slip its content
// (later source frames with +), so the view shows the clip's first frame (left) and last frame (right) with the frames
// slipped; the clip keeps its place and length.
Item {
    id: root

    objectName: "trimView"

    function offsetText(n) {
        return n > 0 ? "+" + n : String(n)
    }
    readonly property bool slip: Trim.type === "slip"

    component Side: Column {
        id: side
        required property string label
        required property string clipName
        required property string timecode
        required property int offset
        required property var image
        property string offsetName: ""

        spacing: Theme.space(1)

        Rectangle {
            width: side.width
            height: Math.min(side.width / Math.max(0.1, ProjectController.frameWidth / Math.max(1, ProjectController.frameHeight)),
                             root.height - buttonsRow.height - Theme.space(16))
            color: Theme.videoBlack
            radius: Theme.radiusClip

            StillFrame {
                anchors.fill: parent
                image: side.image
            }
            Text {
                visible: side.clipName === ""
                anchors.centerIn: parent
                text: qsTr("Gap")
                color: Theme.scrimTextMuted
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
        }
        Row {
            width: side.width
            spacing: Theme.space(2)
            Text {
                width: parent.width - offsetBadge.width - Theme.space(2)
                text: side.clipName !== "" ? qsTr("%1 · %2").arg(side.label).arg(side.clipName) : side.label
                elide: Text.ElideRight
                color: Theme.scrimText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Rectangle {
                id: offsetBadge
                objectName: side.offsetName
                width: Theme.trimViewOffsetWidth
                height: offsetLabel.implicitHeight + Theme.space(1)
                radius: Theme.radiusBadge
                color: side.offset !== 0 ? Qt.alpha(Trim.type === "ripple" ? Theme.trimRipple : Theme.trimRoll, 0.25) : "transparent"
                border.width: Theme.hairline
                border.color: Theme.monitorSurroundBorder
                Text {
                    id: offsetLabel
                    anchors.centerIn: parent
                    text: root.offsetText(side.offset)
                    color: Theme.scrimText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }
        }
        Text {
            text: side.timecode
            color: Theme.scrimTextMuted
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    Row {
        id: sides
        anchors.top: parent.top
        anchors.topMargin: Theme.space(3)
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.space(3)
        // Slip: the clip's first frame (the incoming side) on the left, its last frame on the right.
        layoutDirection: root.slip ? Qt.RightToLeft : Qt.LeftToRight
        readonly property real sideWidth: Math.max(Theme.space(20), (root.width - Theme.space(9)) / 2)

        Side {
            objectName: "trimOutgoing"
            width: sides.sideWidth
            label: root.slip ? qsTr("Last frame") : qsTr("Outgoing")
            clipName: Trim.outgoingName
            timecode: Trim.outgoingText
            offset: Trim.outgoingOffset
            offsetName: "trimOutgoingOffset"
            image: Trim.outgoingFrame
        }
        Side {
            objectName: "trimIncoming"
            width: sides.sideWidth
            label: root.slip ? qsTr("First frame") : qsTr("Incoming")
            clipName: Trim.incomingName
            timecode: Trim.incomingText
            offset: Trim.incomingOffset
            offsetName: "trimIncomingOffset"
            image: Trim.incomingFrame
        }
    }

    Column {
        id: buttonsRow
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space(2)
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.space(2)

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.space(1)

            PillButton {
                objectName: "trimBackwardMany"
                compact: true
                //: Trim view button: trim backward by the Large trim offset, e.g. "−5"
                text: qsTr("−%1").arg(Trim.largeOffset)
                tooltip: qsTr("Trim backward %1 frames").arg(Trim.largeOffset)
                command: "sequence.trimBackwardMany"
                onClicked: Trim.trim(-Trim.largeOffset)
            }
            PillButton {
                objectName: "trimBackward"
                compact: true
                //: Trim view button: trim backward one frame
                text: qsTr("−1")
                tooltip: qsTr("Trim backward one frame")
                command: "sequence.trimBackward"
                onClicked: Trim.trim(-1)
            }
            IconButton {
                objectName: "trimPlayAround"
                compact: true
                checkable: true
                checked: Trim.playingAround
                iconName: "repeat"
                tooltip: qsTr("Play around the edit")
                onClicked: {
                    checked = Qt.binding(() => Trim.playingAround)
                    if (Trim.playingAround)
                        Trim.stopAround()
                    else
                        Trim.playAround()
                }
            }
            PillButton {
                objectName: "trimForward"
                compact: true
                //: Trim view button: trim forward one frame
                text: qsTr("+1")
                tooltip: qsTr("Trim forward one frame")
                command: "sequence.trimForward"
                onClicked: Trim.trim(1)
            }
            PillButton {
                objectName: "trimForwardMany"
                compact: true
                //: Trim view button: trim forward by the Large trim offset, e.g. "+5"
                text: qsTr("+%1").arg(Trim.largeOffset)
                tooltip: qsTr("Trim forward %1 frames").arg(Trim.largeOffset)
                command: "sequence.trimForwardMany"
                onClicked: Trim.trim(Trim.largeOffset)
            }
            IconButton {
                objectName: "trimExit"
                compact: true
                iconName: "x"
                tooltip: qsTr("Leave trim mode")
                onClicked: Trim.clear()
            }
        }
        PillTabs {
            objectName: "trimTypeTabs"
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(Theme.space(64), root.width - Theme.space(4))
            compact: true
            model: [{ label: qsTr("Regular") }, { label: qsTr("Ripple") }, { label: qsTr("Roll") }, { label: qsTr("Slip") }]
            currentIndex: ["regular", "ripple", "roll", "slip"].indexOf(Trim.type)
            onActivated: index => Trim.type = ["regular", "ripple", "roll", "slip"][index]
            Accessible.name: qsTr("Trim type")
        }
    }
}
