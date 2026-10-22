pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// One caption in the Captions tab's list (M5m, C.7): its speaker (colour and name, a menu to assign, add or rename),
// start and end (typed, or nudged a frame at a time), its reading speed (warns above the language's), and its text,
// edited in place. Split at the playhead, join with the next, go to it, delete it. Every change is one undo step.
Rectangle {
    id: row

    required property var modelData
    required property int index
    /// The last cue has no next one to join.
    property bool last: false

    readonly property bool active: Captions.activeCue === modelData.index
    property string speakerEdit: ""   // "" | "new" | "rename"

    objectName: "captionCue_" + modelData.index
    implicitHeight: cueColumn.implicitHeight + Theme.space(2)
    radius: Theme.radiusField
    color: active ? Theme.accentWash : Theme.sunken
    border.width: Theme.hairline
    border.color: active ? Theme.accentStroke : Theme.border

    component TimeField: TextField {
        id: field
        implicitHeight: Theme.controlHeightSmall
        implicitWidth: contentWidth + Theme.space(2)
        Layout.minimumWidth: contentWidth + Theme.space(1)
        Layout.maximumWidth: implicitWidth
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        padding: 0
        selectByMouse: true
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
        color: Theme.textPrimary
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        background: Rectangle {
            radius: Theme.radiusField
            color: field.activeFocus ? Theme.raised : Theme.panel
            border.width: field.activeFocus ? Theme.focusRingWidth : Theme.hairline
            border.color: field.activeFocus ? Theme.accentStroke : Theme.border
        }
    }
    component Nudge: IconButton {
        id: nudge
        property string edge: "start"
        property int frames: 1
        compact: true
        iconSize: Theme.iconXs
        iconName: frames < 0 ? "chevron-left" : "chevron-right"
        tooltip: (frames < 0 ? qsTr("%1 a frame earlier") : qsTr("%1 a frame later"))
                     .arg(edge === "start" ? qsTr("Start") : edge === "end" ? qsTr("End") : qsTr("Caption"))
        onClicked: {
            Captions.nudgeCue(row.modelData.index, nudge.edge, nudge.frames)
            Captions.endEdit()
        }
    }

    ColumnLayout {
        id: cueColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: Theme.space(2)
        spacing: Theme.space(1)

        // Speaker, reading speed and the cue's actions.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                id: speakerPill
                objectName: "captionSpeaker_" + row.modelData.index
                compact: true
                variant: "ghost"
                iconName: "chevron-down"
                text: row.modelData.speakerName !== "" ? row.modelData.speakerName : qsTr("No speaker")
                tooltip: qsTr("Who speaks this caption")
                onClicked: speakerMenu.popup(speakerPill, 0, speakerPill.height + Theme.space(1))
                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.dotSize
                    height: Theme.dotSize
                    radius: Theme.dotSize / 2
                    visible: row.modelData.speakerColor !== ""
                    color: row.modelData.speakerColor !== "" ? row.modelData.speakerColor : "transparent"
                }
                // OptionsMenu, not ContextMenu: this file imports QtQuick.Controls, whose ContextMenu would shadow ours.
                OptionsMenu {
                    id: speakerMenu
                    options: Captions.speakers.map(s => ({ id: s.key, name: s.name }))
                    current: row.modelData.speaker
                    onChosen: id => Captions.setCueSpeaker(row.modelData.index, id)
                    ContextMenuSeparator {}
                    ContextMenuItem {
                        text: qsTr("No speaker")
                        enabled: row.modelData.speaker !== ""
                        onTriggered: Captions.setCueSpeaker(row.modelData.index, "")
                    }
                    ContextMenuItem {
                        objectName: "captionNewSpeaker"
                        text: qsTr("New speaker…")
                        iconName: "plus"
                        onTriggered: row.speakerEdit = "new"
                    }
                    ContextMenuItem {
                        text: qsTr("Rename or recolour…")
                        iconName: "pencil"
                        enabled: row.modelData.speaker !== ""
                        onTriggered: row.speakerEdit = "rename"
                    }
                }
            }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
            // Reading speed: characters a second, warning above the language's comfortable speed.
            Rectangle {
                objectName: "captionCps_" + row.modelData.index
                visible: row.modelData.cps > 0
                implicitWidth: cpsText.implicitWidth + Theme.space(2)
                implicitHeight: cpsText.implicitHeight + Theme.space(0.5)
                radius: Theme.radiusBadge
                color: row.modelData.fast ? Qt.alpha(Theme.warning, 0.16) : "transparent"
                border.width: Theme.hairline
                border.color: row.modelData.fast ? Theme.warning : Theme.border
                Accessible.role: Accessible.StaticText
                Accessible.name: cpsText.text
                Text {
                    id: cpsText
                    anchors.centerIn: parent
                    text: qsTr("%1 cps").arg(Number(row.modelData.cps).toFixed(1))
                    color: row.modelData.fast ? Theme.warning : Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
            IconButton {
                compact: true
                iconName: "play"
                iconSize: Theme.iconXs
                tooltip: qsTr("Go to caption %1").arg(row.modelData.index + 1)
                onClicked: Captions.seekToCue(row.modelData.index)
            }
            IconButton {
                objectName: "captionSplit_" + row.modelData.index
                compact: true
                iconName: "scissors"
                iconSize: Theme.iconXs
                enabled: row.active
                tooltip: qsTr("Split caption %1 at the playhead").arg(row.modelData.index + 1)
                onClicked: Captions.splitCueAtPlayhead(row.modelData.index)
            }
            IconButton {
                objectName: "captionMerge_" + row.modelData.index
                compact: true
                iconName: "fold-vertical"
                iconSize: Theme.iconXs
                enabled: !row.last
                tooltip: qsTr("Join with the next caption")
                onClicked: Captions.mergeWithNext(row.modelData.index)
            }
            IconButton {
                compact: true
                iconName: "x"
                iconSize: Theme.iconXs
                tooltip: qsTr("Delete caption %1").arg(row.modelData.index + 1)
                onClicked: Captions.removeCue(row.modelData.index)
            }
        }

        // New speaker / rename: a name and a colour.
        RowLayout {
            Layout.fillWidth: true
            visible: row.speakerEdit !== ""
            spacing: Theme.space(1)
            FieldInput {
                id: speakerName
                objectName: "captionSpeakerName"
                Layout.fillWidth: true
                Accessible.name: qsTr("Speaker name")
                placeholderText: row.speakerEdit === "new" ? qsTr("Speaker name") : row.modelData.speakerName
                onVisibleChanged: {
                    if (visible) {
                        text = row.speakerEdit === "rename" ? row.modelData.speakerName : ""
                        forceActiveFocus()
                    }
                }
                onAccepted: speakerOk.clicked()
                Keys.onEscapePressed: row.speakerEdit = ""
            }
            Repeater {
                model: row.speakerEdit === "rename" ? Theme.clipColors.slice(0, 6) : []
                delegate: Swatch {
                    required property var modelData
                    swatchColor: modelData
                    checkable: false
                    tooltip: qsTr("Speaker colour")
                    onClicked: Captions.renameSpeaker(row.modelData.speaker, "", String(modelData))
                }
            }
            PillButton {
                id: speakerOk
                objectName: "captionSpeakerOk"
                compact: true
                variant: "primary"
                text: row.speakerEdit === "new" ? qsTr("Add") : qsTr("Rename")
                enabled: speakerName.text.trim() !== ""
                onClicked: {
                    if (row.speakerEdit === "new") {
                        const key = Captions.addSpeaker(speakerName.text)
                        if (key !== "")
                            Captions.setCueSpeaker(row.modelData.index, key)
                    } else {
                        Captions.renameSpeaker(row.modelData.speaker, speakerName.text)
                    }
                    row.speakerEdit = ""
                }
            }
        }

        // Timing: typed, or a frame at a time.
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Nudge { edge: "start"; frames: -1 }
            TimeField {
                id: startField
                objectName: "captionStart_" + row.modelData.index
                text: row.modelData.startText
                Accessible.name: qsTr("Start of caption %1").arg(row.modelData.index + 1)
                onEditingFinished: {
                    if (text !== row.modelData.startText)
                        Captions.setCueTiming(row.modelData.index, text, endField.text)
                }
            }
            Nudge { edge: "start"; frames: 1 }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
            Nudge { edge: "end"; frames: -1 }
            TimeField {
                id: endField
                objectName: "captionEnd_" + row.modelData.index
                text: row.modelData.endText
                Accessible.name: qsTr("End of caption %1").arg(row.modelData.index + 1)
                onEditingFinished: {
                    if (text !== row.modelData.endText)
                        Captions.setCueTiming(row.modelData.index, startField.text, text)
                }
            }
            Nudge { edge: "end"; frames: 1 }
        }

        TextArea {
            id: cueText
            objectName: "captionText"
            Layout.fillWidth: true
            text: row.modelData.text
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            padding: Theme.space(1)
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            Accessible.name: qsTr("Text of caption %1").arg(row.modelData.index + 1)
            // Committed when the field is left (the list is rebuilt after every edit).
            onActiveFocusChanged: {
                if (!activeFocus && text !== row.modelData.text) {
                    Captions.setCueText(row.modelData.index, text)
                    Captions.endEdit()
                }
            }
            background: Rectangle {
                radius: Theme.radiusField
                color: cueText.activeFocus ? Theme.raised : "transparent"
                border.width: cueText.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.focus
            }
            // 0.2: misspelled words underlined in the track's language; right-click for suggestions (and Cut, Copy,
            // Paste: SpellingMenu replaces Qt's own text menu).
            ContextMenu.menu: null
            SpellHighlighter {
                objectName: "captionSpelling"
                document: cueText.textDocument
                language: Captions.trackInfo.language ?? ""
                color: Theme.danger
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => cueSpelling.openAt(eventPoint.position.x, eventPoint.position.y)
            }
            SpellingMenu {
                id: cueSpelling
                target: cueText
                language: Captions.trackInfo.language ?? ""
                onCorrected: text => {
                    cueText.text = text
                    Captions.setCueText(row.modelData.index, text)
                    Captions.endEdit()
                }
            }
        }
    }
}
