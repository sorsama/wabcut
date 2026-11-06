pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Wabcut's Media Encoder (0.2; Premiere's Send to Media Encoder, Alt+Shift+M): the export queue - every job with its
// progress, Start Queue for the ones sent to it, pause / resume / cancel per job - its Watch Folders (media dropped into
// a folder is exported with a preset into its Output folder, the source then moves into Source), and the CPU limit
// preference for exports. Everything runs on ExportController's queue, one export at a time, in the background.
// 0.7 (card 004): the History tab lists the exports that finished (the last 100, kept between sessions) with Show, Open
// and Export Again (the same preset and settings on the project as it is now).
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
    property string historyError: ""
    readonly property var presetList: ExportController.presets.filter(p => p.category !== "hdr")
    function openOn(page) {
        pageTabs.currentIndex = page === "watch" ? 1 : page === "history" ? 2 : 0
        if (page === "history")
            ExportController.refreshHistory()
        dialog.historyError = ""
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
        Accessible.name: qsTr("Export queue page") // P1 #61
        Layout.fillWidth: true
        compact: true
        tabs: true
        model: [qsTr("Queue"), qsTr("Watch Folders"), qsTr("History")]
        onCurrentIndexChanged: {
            if (currentIndex === 2)
                ExportController.refreshHistory()
        }
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
                // The notice follows the last Open: gone after it works again (a drive plugged back in).
                onOpenRequested: job.notice = ExportController.openOutput(job.jobId)
                                 ? "" : qsTr("The file was moved or deleted.")
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

    // ---- History (0.7, card 004) ----
    ColumnLayout {
        objectName: "exportHistoryPage"
        visible: pageTabs.currentIndex === 2
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Text {
            objectName: "exportHistoryEmpty"
            Layout.fillWidth: true
            visible: ExportController.history.length === 0
            text: qsTr("No finished exports yet. Each export that finishes is listed here, so you can show it, open it, or export it again with the same settings.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        ListView {
            id: historyList
            objectName: "exportHistoryList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.space(90))
            visible: count > 0
            clip: true
            spacing: Theme.space(2)
            boundsBehavior: Flickable.StopAtBounds
            model: ExportController.history
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Export history")

            delegate: Rectangle {
                id: entry
                required property var modelData
                required property int index
                objectName: "exportHistoryRow_" + entry.index
                width: historyList.width - (historyList.ScrollBar.vertical && historyList.ScrollBar.vertical.visible ? Theme.space(2) : 0)
                implicitHeight: entryRow.implicitHeight + Theme.space(3)
                height: implicitHeight
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.ListItem
                Accessible.name: entry.modelData.exists ? entry.modelData.title : qsTr("%1, the file is gone").arg(entry.modelData.title)

                RowLayout {
                    id: entryRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.space(3)
                    anchors.rightMargin: Theme.space(2)
                    spacing: Theme.space(2)

                    Icon {
                        Layout.alignment: Qt.AlignVCenter
                        name: entry.modelData.exists ? "film" : "triangle-alert"
                        size: Theme.iconSm
                        color: entry.modelData.exists ? Theme.accentStroke : Theme.warning
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: Theme.space(30)
                        spacing: 0
                        Text {
                            objectName: "exportHistoryTitle_" + entry.index
                            Layout.fillWidth: true
                            // The file name greyed when it is gone.
                            text: entry.modelData.exists ? entry.modelData.title : qsTr("%1 (file not found)").arg(entry.modelData.title)
                            elide: Text.ElideMiddle
                            color: entry.modelData.exists ? Theme.textPrimary : Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Font.Medium
                        }
                        Text {
                            Layout.fillWidth: true
                            text: [entry.modelData.presetName, entry.modelData.sequenceName, entry.modelData.whenText]
                                      .filter(t => t !== "").join(" · ")
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    PillButton {
                        objectName: "exportHistoryShow_" + entry.index
                        compact: true
                        variant: "ghost"
                        iconName: "folder-open"
                        text: qsTr("Show")
                        tooltip: entry.modelData.exists ? qsTr("Show the file in %1").arg(ExportController.fileManagerName)
                                                        : qsTr("The file is gone: show its folder in %1").arg(ExportController.fileManagerName)
                        onClicked: dialog.historyError = ExportController.revealHistory(entry.index) ? "" : qsTr("The folder is gone too.")
                    }
                    PillButton {
                        objectName: "exportHistoryOpen_" + entry.index
                        compact: true
                        variant: "ghost"
                        iconName: "play"
                        text: qsTr("Open")
                        enabled: entry.modelData.exists
                        tooltip: qsTr("Open the file with the system's player")
                        onClicked: dialog.historyError = ExportController.openHistory(entry.index) ? "" : qsTr("The file was moved or deleted.")
                    }
                    PillButton {
                        objectName: "exportHistoryAgain_" + entry.index
                        compact: true
                        variant: "secondary"
                        iconName: "repeat"
                        text: qsTr("Export Again")
                        enabled: entry.modelData.canRepeat
                        tooltip: entry.modelData.canRepeat ? qsTr("Export again with the same preset and settings")
                                                           : qsTr("This export has no preset to repeat: export it from the Export panel")
                        onClicked: {
                            const job = ExportController.exportAgain(entry.index)
                            dialog.historyError = job !== "" ? "" : ExportController.lastError
                            if (job !== "")
                                pageTabs.currentIndex = 0
                        }
                    }
                    IconButton {
                        objectName: "exportHistoryRemove_" + entry.index
                        compact: true
                        iconName: "x"
                        tooltip: qsTr("Remove from the history")
                        onClicked: ExportController.removeHistory(entry.index)
                    }
                }
            }
        }

        Text {
            objectName: "exportHistoryError"
            Layout.fillWidth: true
            visible: dialog.historyError !== ""
            text: dialog.historyError
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        RowLayout {
            Layout.fillWidth: true
            visible: ExportController.history.length > 0
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "exportHistoryClear"
                variant: "ghost"
                text: qsTr("Clear History")
                onClicked: ExportController.clearHistory()
            }
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
