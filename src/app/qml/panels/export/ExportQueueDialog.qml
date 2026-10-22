pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Wabcut's Media Encoder (0.2; Premiere's Send to Media Encoder, Alt+Shift+M): the export queue - every job with its
// progress, Start Queue for the ones sent to it, pause / resume / cancel per job - its Watch Folders (media dropped into
// a folder is exported with a preset into its Output folder, the source then moves into Source), and the CPU limit
// preference for exports. Everything runs on ExportController's queue, one export at a time, in the background.
WabcutDialog {
    id: dialog
    objectName: "exportQueueDialog"

    title: qsTr("Export Queue")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    readonly property var cpuLimits: [100, 75, 50, 25]
    property string watchPreset: "match-source"
    property string watchError: ""
    property var picked: []
    property string addError: ""
    readonly property var presetList: ExportController.presets.filter(p => p.category !== "hdr")
    function openOn(page) {
        pageTabs.currentIndex = page === "watch" ? 1 : 0
        dialog.open()
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    PillTabs {
        id: pageTabs
        objectName: "exportQueuePages"
        Layout.fillWidth: true
        compact: true
        tabs: true
        model: [qsTr("Queue"), qsTr("Watch Folders")]
    }

    // ---- Queue ----
    ColumnLayout {
        visible: pageTabs.currentIndex === 0
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Text {
            Layout.fillWidth: true
            visible: ExportController.jobs.count === 0
            text: qsTr("Nothing in the queue. Export, or Send to Media Encoder from the Export workspace, adds exports here.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        ListView {
            id: queueList
            objectName: "exportQueueList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.space(90))
            visible: count > 0
            clip: true
            spacing: Theme.space(2)
            boundsBehavior: Flickable.StopAtBounds
            model: ExportController.jobs
            ScrollBar.vertical: ThinScrollBar {}

            delegate: ExportJobPill {
                id: job
                width: queueList.width
                objectName: "exportQueueJob_" + job.jobId
                onCancelRequested: ExportController.cancel(job.jobId)
                onRevealRequested: ExportController.reveal(job.jobId)
                onPauseRequested: ExportController.pause(job.jobId)
                onResumeRequested: ExportController.resume(job.jobId)
                onStartRequested: ExportController.start(job.jobId)
            }
        }

        // Several destinations at once (Premiere's Export mode): the current sequence with every preset picked here.
        GroupLabel { text: qsTr("Add the current sequence with these presets") }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Repeater {
                model: dialog.presetList
                delegate: PillButton {
                    id: pick
                    required property var modelData
                    objectName: "exportQueuePick_" + pick.modelData.id
                    compact: true
                    checkable: true
                    variant: checked ? "primary" : "secondary"
                    text: pick.modelData.name
                    onToggled: {
                        const next = dialog.picked.filter(id => id !== pick.modelData.id)
                        if (checked)
                            next.push(pick.modelData.id)
                        dialog.picked = next
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "exportQueueAdd"
                variant: "secondary"
                iconName: "plus"
                text: dialog.picked.length > 1 ? qsTr("Add %1 exports").arg(dialog.picked.length) : qsTr("Add export")
                enabled: dialog.picked.length > 0
                onClicked: {
                    const jobs = ExportController.sendToQueue("", dialog.picked, {}, holdToggle.checked)
                    dialog.addError = jobs.length > 0 ? "" : ExportController.lastError
                }
            }
            Toggle {
                id: holdToggle
                text: qsTr("Wait for Start Queue")
                checked: true
            }
            Text {
                Layout.fillWidth: true
                visible: dialog.addError !== ""
                text: dialog.addError
                elide: Text.ElideRight
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "exportQueueStart"
                variant: "primary"
                iconName: "play"
                text: qsTr("Start Queue")
                enabled: ExportController.jobs.activeCount > 0
                onClicked: ExportController.startQueue()
            }
            PillButton {
                objectName: "exportQueueStop"
                variant: "secondary"
                iconName: "square"
                text: qsTr("Stop All")
                enabled: ExportController.jobs.activeCount > 0
                onClicked: ExportController.cancelAll()
            }
            Item { Layout.fillWidth: true }
            PillButton {
                variant: "ghost"
                text: qsTr("Clear Finished")
                visible: ExportController.jobs.count > ExportController.jobs.activeCount
                onClicked: ExportController.clearFinished()
            }
        }
    }

    // ---- Watch Folders ----
    ColumnLayout {
        visible: pageTabs.currentIndex === 1
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Text {
            Layout.fillWidth: true
            text: qsTr("Media files dropped into a watched folder are exported with its preset into its Output folder, one after another; each source then moves into the folder's Source folder.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Repeater {
            objectName: "exportWatchFolders"
            model: ExportController.watchFolders
            delegate: Rectangle {
                id: folderRow
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: folderColumn.implicitHeight + Theme.space(3)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(3)
                    anchors.rightMargin: Theme.space(2)
                    spacing: Theme.space(2)
                    Icon {
                        name: "folder-open"
                        size: Theme.iconSm
                        color: Theme.accentStroke
                    }
                    ColumnLayout {
                        id: folderColumn
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: folderRow.modelData.folder
                            elide: Text.ElideMiddle
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 → %2 · %3 queued · %4 done · %5 failed")
                                      .arg(folderRow.modelData.presetName).arg(folderRow.modelData.output)
                                      .arg(folderRow.modelData.queued).arg(folderRow.modelData.done).arg(folderRow.modelData.failed)
                                  + (folderRow.modelData.lastError !== "" ? " · " + folderRow.modelData.lastError : "")
                            elide: Text.ElideMiddle
                            color: folderRow.modelData.lastError !== "" ? Theme.warning : Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    IconButton {
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Stop watching this folder")
                        onClicked: ExportController.removeWatchFolder(folderRow.modelData.id)
                    }
                }
            }
        }

        GroupLabel { text: qsTr("Add a watch folder") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                objectName: "exportWatchFolderPath"
                text: folderDialog.selectedFolder.toString() === "" ? qsTr("Choose a folder")
                                                                     : folderDialog.selectedFolder.toString().replace(/^file:\/\/\/?/, "")
                elide: Text.ElideMiddle
                color: folderDialog.selectedFolder.toString() === "" ? Theme.textTertiary : Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            PillButton {
                compact: true
                text: qsTr("Folder…")
                onClicked: folderDialog.open()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                id: watchPresetButton
                objectName: "exportWatchPreset"
                Layout.fillWidth: true
                compact: true
                variant: "secondary"
                text: (dialog.presetList.find(p => p.id === dialog.watchPreset) ?? { name: dialog.watchPreset }).name
                iconName: "chevron-down"
                LayoutMirroring.enabled: true
                LayoutMirroring.childrenInherit: true
                onClicked: watchPresetMenu.popup(watchPresetButton, 0, watchPresetButton.height + Theme.space(1))
            }
            Toggle {
                id: moveSources
                text: qsTr("Move sources to Source")
                checked: true
            }
            PillButton {
                objectName: "exportWatchAdd"
                variant: "primary"
                compact: true
                iconName: "folder-plus"
                text: qsTr("Watch")
                enabled: folderDialog.selectedFolder.toString() !== ""
                onClicked: {
                    const r = ExportController.addWatchFolder(folderDialog.selectedFolder.toString(), dialog.watchPreset, "",
                                                              moveSources.checked)
                    dialog.watchError = r.ok ? "" : r.error
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.watchError !== ""
            text: dialog.watchError
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // ---- Performance ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        GroupLabel { text: qsTr("Limit CPU use") }
        PillTabs {
            id: cpuTabs
            objectName: "exportCpuLimit"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("No limit"), qsTr("75 %"), qsTr("50 %"), qsTr("25 %")]
            currentIndex: Math.max(0, dialog.cpuLimits.indexOf(ExportController.cpuLimit))
            Accessible.name: qsTr("Limit CPU use for exports")
            onActivated: index => ExportController.cpuLimit = dialog.cpuLimits[index]
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Watch folder")
    }

    // A menu of its own file (QtQuick only): Controls' Menu would shadow Wabcut's ContextMenu here.
    ExportPresetMenu {
        id: watchPresetMenu
        presets: dialog.presetList
        current: dialog.watchPreset
        onChosen: id => dialog.watchPreset = id
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
