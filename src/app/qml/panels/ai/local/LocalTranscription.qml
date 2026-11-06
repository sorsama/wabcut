pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Settings > AI > Local models > Transcription: the whisper.cpp model that "On this computer" transcription uses.
// "Automatic" picks large-v3-turbo (5-bit) with a GPU and base without one (or whichever of them is downloaded).
// Choosing a model that is not downloaded downloads it (with the 0.9 MB voice detector) and switches to it when done;
// a transcription started before then downloads the model it needs first.
Column {
    id: root
    objectName: "localTranscription"

    spacing: Theme.space(3)

    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("Transcription on this computer uses OpenAI's Whisper through whisper.cpp: no audio leaves it. Bigger models are more accurate and slower; English-only models are a little better at English.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Text {
        objectName: "localWhisperCurrent"
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("Now: %1").arg(LocalModels.whisperModelName)
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }

    // Automatic.
    Rectangle {
        objectName: "localWhisper_auto"
        width: parent.width
        implicitHeight: autoRow.implicitHeight + Theme.space(6)
        radius: Theme.radiusCard
        color: Theme.section
        border.width: LocalModels.whisperModel === "" ? 2 : Theme.hairline
        border.color: LocalModels.whisperModel === "" ? Theme.accentStroke : Theme.border

        Flow {
            id: autoRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)

            Text {
                width: Math.min(implicitWidth, autoRow.width)
                wrapMode: Text.WordWrap
                text: qsTr("Automatic: the best model for this computer")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            LocalTag {
                visible: LocalModels.whisperModel === ""
                text: qsTr("Selected")
                tone: "accent"
            }
            PillButton {
                objectName: "localWhisperAutomatic"
                compact: true
                visible: LocalModels.whisperModel !== ""
                text: qsTr("Use automatic")
                onClicked: LocalModels.useForTranscription("")
            }
        }
    }

    Repeater {
        model: LocalModels.whisperModels

        delegate: Rectangle {
            id: whisper
            required property var modelData
            readonly property var download: LocalModels.downloads.find(d => d.id === whisper.modelData.id) ?? null
            readonly property bool downloadActive: whisper.download !== null && whisper.download.status !== "done" && whisper.download.status !== "failed"

            objectName: "localWhisper_" + modelData.id
            width: root.width
            implicitHeight: whisperColumn.implicitHeight + Theme.space(6)
            radius: Theme.radiusCard
            color: Theme.section
            border.width: modelData.selected ? 2 : Theme.hairline
            border.color: modelData.selected ? Theme.accentStroke : Theme.border
            Accessible.role: Accessible.Grouping
            Accessible.name: modelData.name

            Column {
                id: whisperColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(3)
                spacing: Theme.space(1.5)

                Flow {
                    width: parent.width
                    spacing: Theme.space(1.5)

                    Text {
                        width: Math.min(implicitWidth, whisperColumn.width)
                        wrapMode: Text.WordWrap
                        text: whisper.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    LocalTag {
                        visible: whisper.modelData.selected
                        text: qsTr("Selected")
                        tone: "accent"
                    }
                    LocalTag {
                        visible: whisper.modelData.active && !whisper.modelData.selected
                        text: qsTr("Used now")
                        tone: "success"
                    }
                    LocalTag {
                        visible: whisper.modelData.recommended
                        text: qsTr("Best for this computer")
                    }
                    LocalTag {
                        visible: whisper.modelData.installed
                        text: qsTr("Downloaded")
                        tone: "success"
                    }
                    LocalTag {
                        visible: whisper.modelData.englishOnly
                        text: qsTr("English only")
                    }
                }

                Text {
                    visible: text !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: whisper.modelData.note
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                BeamProgress {
                    visible: whisper.download !== null && whisper.download.status !== "done"
                    width: parent.width
                    value: whisper.download ? whisper.download.progress : 0
                    active: whisper.download !== null && whisper.download.status === "running"
                }

                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    PillButton {
                        objectName: "localWhisperUse"
                        compact: true
                        variant: whisper.modelData.selected ? "secondary" : "primary"
                        visible: !whisper.modelData.selected && !whisper.downloadActive
                        enabled: whisper.modelData.installed || !LocalModels.offline
                        disabledReason: qsTr("Offline mode is on")
                        text: whisper.modelData.installed ? qsTr("Use") : qsTr("Download and use (%1)").arg(whisper.modelData.size)
                        Accessible.name: text + " " + whisper.modelData.name
                        onClicked: {
                            const message = LocalModels.useForTranscription(whisper.modelData.id)
                            if (message !== "")
                                LocalModels.notice(message)
                        }
                    }
                }
            }
        }
    }
}
