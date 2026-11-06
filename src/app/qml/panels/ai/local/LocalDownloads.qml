pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The download queue of the local model library (Settings > AI > Local models): one row per model with its progress
// (bytes, speed, time left), Pause / Resume and Cancel, and why it stopped when it failed. Hidden while nothing is
// queued. Downloads continue while the Settings window is closed; a paused one resumes where it stopped.
Rectangle {
    id: root
    objectName: "localDownloads"

    visible: LocalModels.downloads.length > 0
    implicitHeight: visible ? column.implicitHeight + Theme.space(6) : 0
    radius: Theme.radiusCard
    color: Theme.section
    border.width: Theme.hairline
    border.color: Theme.border
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Downloads")

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(3)
        spacing: Theme.space(3)

        Item {
            width: parent.width
            height: Math.max(heading.implicitHeight, clearButton.height)

            Text {
                id: heading
                anchors.left: parent.left
                anchors.right: clearButton.left
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                text: qsTr("Downloads")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.Bold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            PillButton {
                id: clearButton
                objectName: "localDownloadsClear"
                anchors.right: parent.right
                compact: true
                visible: LocalModels.downloads.some(d => d.status === "done" || d.status === "failed")
                text: qsTr("Clear finished")
                onClicked: LocalModels.clearFinished()
            }
        }

        Repeater {
            model: LocalModels.downloads

            delegate: Column {
                id: item
                required property var modelData
                objectName: "localDownload_" + modelData.id
                width: column.width
                spacing: Theme.space(1.5)

                readonly property string statusText: modelData.status === "running" ? qsTr("Downloading")
                                                   : modelData.status === "queued" ? qsTr("Waiting")
                                                   : modelData.status === "paused" ? qsTr("Paused")
                                                   : modelData.status === "done" ? qsTr("Done")
                                                   : qsTr("Stopped")

                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        width: Math.min(implicitWidth, item.width)
                        elide: Text.ElideRight
                        text: item.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                    }
                    LocalTag {
                        text: item.statusText
                        tone: item.modelData.status === "failed" ? "danger" : item.modelData.status === "done" ? "success" : "neutral"
                    }
                }

                BeamProgress {
                    width: parent.width
                    value: item.modelData.progress
                    active: item.modelData.status === "running"
                    hue: item.modelData.status === "failed" ? Theme.danger : Theme.accentFill
                    Accessible.name: qsTr("%1 downloaded").arg(item.modelData.name)
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: item.modelData.status === "failed" ? item.modelData.error
                          : [item.modelData.receivedText, item.modelData.fileText, item.modelData.speedText, item.modelData.etaText]
                                .filter(s => s !== "").join(" · ")
                    color: item.modelData.status === "failed" ? Theme.dangerText : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }

                Flow {
                    width: parent.width
                    spacing: Theme.space(2)
                    visible: item.modelData.status !== "done"

                    PillButton {
                        objectName: "localDownloadPause"
                        compact: true
                        visible: item.modelData.canPause
                        iconName: "pause"
                        text: qsTr("Pause")
                        Accessible.name: qsTr("Pause %1").arg(item.modelData.name)
                        onClicked: LocalModels.pause(item.modelData.id)
                    }
                    PillButton {
                        objectName: "localDownloadResume"
                        compact: true
                        visible: item.modelData.canResume
                        enabled: !LocalModels.offline
                        iconName: "download"
                        text: item.modelData.status === "failed" ? qsTr("Try again") : qsTr("Resume")
                        disabledReason: qsTr("Offline mode is on")
                        Accessible.name: text + " " + item.modelData.name
                        onClicked: LocalModels.resume(item.modelData.id)
                    }
                    PillButton {
                        objectName: "localDownloadCancel"
                        compact: true
                        variant: "ghost"
                        iconName: "x"
                        text: qsTr("Cancel")
                        Accessible.name: qsTr("Cancel %1").arg(item.modelData.name)
                        onClicked: LocalModels.cancel(item.modelData.id)
                    }
                }
            }
        }
    }
}
