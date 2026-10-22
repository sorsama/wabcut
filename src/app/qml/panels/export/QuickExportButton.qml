pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Premiere's Quick Export (0.2): the header's one-click export of the current sequence. A popover picks the preset
// (built-in or the user's), shows what it writes, and the file name and location (default: next to the project, named
// after the sequence); Export queues it on ExportController like the Export workspace does, so it renders in the
// background while editing goes on.
IconButton {
    id: root
    objectName: "quickExportButton"

    property string presetId: "match-source"
    property string output: ""
    readonly property var presetList: ExportController.presets.filter(p => p.category !== "hdr")
    readonly property var summary: popover.opened && ProjectController.revision >= 0 && ExportController.jobs.count >= 0
                                   && ExportController.autoEncoders
                                   ? ExportController.describe(ProjectController.sequenceId, presetId,
                                                               output !== "" ? { output: output } : {})
                                   : ({})

    function exportNow() {
        if (ExportController.quickExport(root.presetId, root.output) !== "") {
            root.output = ""
            popover.close()
        }
    }

    compact: true
    iconName: "zap"
    tooltip: qsTr("Quick Export")
    checkable: true
    checked: popover.opened
    onClicked: {
        checked = Qt.binding(() => popover.opened)
        if (popover.opened)
            popover.close()
        else
            popover.open()
    }

    component FieldLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Popover {
        id: popover
        objectName: "quickExportPopover"
        title: qsTr("Quick Export")
        x: root.width - width
        y: root.height + Theme.space(2)
        transformOrigin: Item.TopRight
        onAboutToShow: ExportController.probeEncoders()

        Column {
            width: Theme.space(92)
            spacing: Theme.space(2)

            FieldLabel { text: qsTr("Preset") }
            ListView {
                id: presetView
                objectName: "quickExportPresets"
                width: parent.width
                height: Theme.space(56)
                clip: true
                spacing: Theme.space(1)
                boundsBehavior: Flickable.StopAtBounds
                model: root.presetList
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property bool current: modelData.id === root.presetId
                    width: presetView.width
                    height: Theme.controlHeight
                    radius: height / 2
                    color: current ? Theme.accentWash : rowArea.containsMouse ? Theme.hover : "transparent"
                    objectName: "quickExportPreset_" + modelData.id
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checked: current

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(3)
                        anchors.rightMargin: Theme.space(3)
                        spacing: Theme.space(2)
                        Icon {
                            name: row.modelData.icon
                            size: Theme.iconSm
                            color: row.current ? Theme.accentStroke : Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: row.current ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            visible: row.modelData.user
                            text: qsTr("Mine")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.presetId = row.modelData.id
                    }
                }
            }

            Text {
                objectName: "quickExportSummary"
                width: parent.width
                text: root.summary.ok === true ? qsTr("%1 · %2 · %3").arg(root.summary.format).arg(root.summary.resolution)
                                                       .arg(root.summary.duration)
                                               : (root.summary.error ?? "")
                wrapMode: Text.WordWrap
                color: root.summary.ok === false ? Theme.danger : Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }

            FieldLabel { text: qsTr("File Name & Location") }
            Row {
                width: parent.width
                spacing: Theme.space(2)
                Text {
                    objectName: "quickExportOutput"
                    width: parent.width - browse.width - parent.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.summary.output ?? ""
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    id: browse
                    compact: true
                    text: qsTr("Browse…")
                    onClicked: saveDialog.open()
                }
            }

            Text {
                width: parent.width
                visible: text !== ""
                text: ExportController.lastError
                wrapMode: Text.WordWrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            PillButton {
                objectName: "quickExportGo"
                width: parent.width
                variant: "primary"
                iconName: "upload"
                text: qsTr("Export")
                enabled: root.summary.ok === true
                onClicked: root.exportNow()
            }
        }
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Quick Export to")
        fileMode: FileDialog.SaveFile
        onAccepted: root.output = selectedFile.toString()
    }
}
