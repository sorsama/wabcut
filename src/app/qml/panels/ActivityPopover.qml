pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Background tasks (0.4): the title bar's activity popover (TopBar's activity pill, window.backgroundTasks). Premiere's
// Progress dashboard and Events panel in one place:
//   1. Exports (while the queue has any): one ExportJobPill per job (`activityExport_<jobId>`) with the queue's own
//      buttons (pause / resume / start, cancel, Open, Show), and Start Queue (held exports), Clear Finished, Export
//      Queue… (the queue in the Export workspace: exportQueueRequested).
//   2. The other background work, one row each with its progress and a stop: Render In to Out, proxies (Settings…),
//      transcription, Highlights, Shorts, Collect Files, an extension's AI command.
//   3. "Nothing is running." when none of that runs.
//   4. Recent messages (MessageLog, newest first): tone dot, time, the text (two lines; a click on a longer one shows
//      all of it) and its action, if it had one (runs it and closes the popover); Clear. Opening the popover marks the
//      errors seen.
// No menus here (QtQuick.Controls is imported for the scroll bars only).
Popover {
    id: root
    objectName: "activityPopover"

    title: qsTr("Background tasks")

    /// Export Queue…: the queue in the Export workspace (Main.showExportQueue).
    signal exportQueueRequested()

    readonly property ExportJobModel jobs: ExportController.jobs
    readonly property var longCommand: Extensions.longCommand
    readonly property bool othersRunning: MediaReadiness.rendering || MediaReadiness.working
                                          || Transcript.jobStatus === "running" || Highlights.busy || Shorts.busy
                                          || Collect.running || root.longCommand.running === true
    readonly property bool nothingRuns: (root.jobs === null || root.jobs.count === 0) && !root.othersRunning

    onOpened: MessageLog.markSeen()

    /// "38 %" for 0..1; "" while unknown (-1).
    function percentText(value: real): string {
        return value >= 0 ? qsTr("%1 %").arg(Math.round(Math.min(1, value) * 100)) : ""
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    // A background job that is not an export: an icon, its name and what it does, its progress (a bar; a moving one
    // while the progress is unknown) and one button (Stop, Cancel, Settings…).
    component JobRow: Rectangle {
        id: row

        property string iconName: "zap"
        property string label: ""
        property string detail: ""
        /// 0..1, or -1 while unknown.
        property real progress: -1
        property string actionText: ""
        signal actionClicked()

        Layout.fillWidth: true
        implicitHeight: rowColumn.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        Accessible.role: Accessible.ProgressBar
        Accessible.name: row.label
        Accessible.description: [row.detail, root.percentText(row.progress)].filter(s => s !== "").join(" · ")

        ColumnLayout {
            id: rowColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Theme.space(3)
            anchors.rightMargin: Theme.space(2)
            spacing: Theme.space(1.5)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)

                Icon {
                    name: row.iconName
                    size: Theme.iconSm
                    color: Theme.accentStroke
                }
                Text {
                    Layout.fillWidth: true
                    text: row.detail !== "" ? qsTr("%1 · %2").arg(row.label).arg(row.detail) : row.label
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
                Text {
                    visible: row.progress >= 0
                    text: root.percentText(row.progress)
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    objectName: row.objectName + "Action"
                    visible: row.actionText !== ""
                    compact: true
                    variant: "ghost"
                    text: row.actionText
                    Accessible.name: qsTr("%1: %2").arg(row.actionText).arg(row.label)
                    onClicked: row.actionClicked()
                }
            }
            BeamProgress {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.progressHeight
                Layout.rightMargin: Theme.space(1)
                value: Math.max(0, row.progress)
                indeterminate: row.progress < 0
                active: row.visible && root.visible
                label: row.label
            }
        }
    }

    ColumnLayout {
        id: content
        width: Theme.space(116)
        spacing: Theme.space(3)

        // ---- 1. Exports ----
        ColumnLayout {
            objectName: "activityExports"
            Layout.fillWidth: true
            visible: root.jobs !== null && root.jobs.count > 0
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)

                GroupLabel {
                    Layout.fillWidth: true
                    text: qsTr("Exports")
                }
                PillButton {
                    objectName: "activityStartQueue"
                    visible: root.jobs !== null && root.jobs.heldCount > 0
                    compact: true
                    variant: "secondary"
                    iconName: "play"
                    text: qsTr("Start Queue")
                    onClicked: ExportController.startQueue()
                }
                PillButton {
                    objectName: "activityClearFinished"
                    visible: root.jobs !== null && root.jobs.count > root.jobs.activeCount
                    compact: true
                    variant: "ghost"
                    text: qsTr("Clear Finished")
                    onClicked: ExportController.clearFinished()
                }
                PillButton {
                    objectName: "activityOpenQueue"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Export Queue…")
                    tooltip: qsTr("The export queue in the Export workspace")
                    onClicked: {
                        root.close()
                        root.exportQueueRequested()
                    }
                }
            }
            // The jobs on a sunken well (the pills are raised, like the popover).
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(exportColumn.implicitHeight, Theme.space(64)) + Theme.space(4)
                radius: Theme.radiusCard
                color: Theme.sunken

                Flickable {
                    id: exportFlick
                    objectName: "activityExportList"
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    clip: true
                    contentWidth: width
                    contentHeight: exportColumn.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar {}

                    Column {
                        id: exportColumn
                        width: exportFlick.width
                        spacing: Theme.space(2)

                        Repeater {
                            model: root.jobs
                            delegate: ExportJobPill {
                                id: job
                                width: exportColumn.width
                                objectName: "activityExport_" + job.jobId
                                onCancelRequested: ExportController.cancel(job.jobId)
                                onRevealRequested: ExportController.reveal(job.jobId)
                                // The notice follows the last Open: gone after it works again.
                                onOpenRequested: job.notice = ExportController.openOutput(job.jobId)
                                                 ? "" : qsTr("The file was moved or deleted.")
                                onPauseRequested: ExportController.pause(job.jobId)
                                onResumeRequested: ExportController.resume(job.jobId)
                                onStartRequested: ExportController.start(job.jobId)
                            }
                        }
                    }
                }
            }
        }

        // ---- 2. The other background work ----
        JobRow {
            objectName: "activityRender"
            visible: MediaReadiness.rendering
            iconName: "film"
            label: qsTr("Render In to Out")
            progress: MediaReadiness.renderProgress
            actionText: qsTr("Stop")
            onActionClicked: MediaReadiness.cancelRender()
        }
        JobRow {
            objectName: "activityProxies"
            visible: MediaReadiness.working
            iconName: "layers"
            label: MediaReadiness.statusText !== "" ? MediaReadiness.statusText : qsTr("Proxies")
            detail: MediaReadiness.statusDetail
            actionText: qsTr("Settings…")
            onActionClicked: {
                root.close()
                MediaReadiness.openSettings()
            }
        }
        JobRow {
            objectName: "activityTranscription"
            visible: Transcript.jobStatus === "running"
            iconName: "captions"
            label: qsTr("Transcribing")
            detail: Transcript.jobText
            progress: Transcript.jobProgress
            actionText: qsTr("Cancel")
            onActionClicked: Transcript.cancelTranscription()
        }
        JobRow {
            objectName: "activityHighlights"
            visible: Highlights.busy
            iconName: "sparkles"
            label: qsTr("Highlights")
            detail: Highlights.activity
            progress: Highlights.progress
            actionText: qsTr("Cancel")
            onActionClicked: Highlights.cancel()
        }
        JobRow {
            objectName: "activityShorts"
            visible: Shorts.busy
            iconName: "clapperboard"
            label: qsTr("Shorts")
            detail: Shorts.phase
            progress: Shorts.progress
            actionText: qsTr("Cancel")
            onActionClicked: Shorts.cancel()
        }
        JobRow {
            objectName: "activityCollect"
            visible: Collect.running
            iconName: "folders"
            label: qsTr("Collecting files")
            detail: Collect.phase
            progress: Collect.progress
            actionText: qsTr("Cancel")
            onActionClicked: Collect.cancel()
        }
        JobRow {
            objectName: "activityExtension"
            visible: root.longCommand.running === true
            iconName: "puzzle"
            label: String(root.longCommand.extension ?? "")
            detail: (root.longCommand.queued ?? 0) > 0
                    ? qsTr("%1 (%n more waiting)", "", root.longCommand.queued).arg(String(root.longCommand.text ?? ""))
                    : String(root.longCommand.text ?? "")
            progress: root.longCommand.progress ?? -1
            actionText: qsTr("Stop")
            onActionClicked: Extensions.cancelLongCommand()
        }

        // ---- 3. Nothing ----
        Text {
            objectName: "activityNothing"
            Layout.fillWidth: true
            visible: root.nothingRuns
            text: qsTr("Nothing is running.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        // ---- 4. Recent messages ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hairline
            color: Theme.border
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)

            GroupLabel {
                Layout.fillWidth: true
                text: qsTr("Recent messages")
            }
            PillButton {
                objectName: "activityClearMessages"
                visible: MessageLog.count > 0
                compact: true
                variant: "ghost"
                text: qsTr("Clear")
                tooltip: qsTr("Forget the recent messages")
                onClicked: MessageLog.clear()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: MessageLog.count === 0
            text: qsTr("Messages from exports, saves and other work show here.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        ListView {
            id: messageList
            objectName: "activityMessages"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.space(72))
            visible: MessageLog.count > 0
            clip: true
            spacing: Theme.space(1)
            boundsBehavior: Flickable.StopAtBounds
            model: MessageLog.entries
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Recent messages")

            delegate: Item {
                id: entry
                required property var modelData
                required property int index
                readonly property string tone: String(entry.modelData.tone)
                readonly property bool hasAction: String(entry.modelData.actionText ?? "") !== ""
                                                  && typeof entry.modelData.action === "function"
                /// A long message shows two lines (the pointer over it turns into a hand): a click shows all of it, a
                /// second click two lines again. A screen reader reads it whole (Accessible.name).
                property bool expanded: false
                readonly property bool cut: messageText.truncated
                function toggleExpanded() {
                    if (entry.cut || entry.expanded)
                        entry.expanded = !entry.expanded
                }
                objectName: "activityMessage_" + entry.index
                width: messageList.width
                implicitHeight: Math.max(messageText.implicitHeight, entryAction.visible ? entryAction.implicitHeight : 0)
                                + Theme.space(2)
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, %2").arg(timeText.text).arg(String(entry.modelData.text))

                // The dot and the time sit on the text's first line; the text and its action are centred in the row.
                Rectangle {
                    id: toneDot
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(1)
                    y: messageText.y + (messageMetrics.height - height) / 2
                    width: Theme.dotSize
                    height: Theme.dotSize
                    radius: width / 2
                    color: entry.tone === "danger" ? Theme.danger
                         : entry.tone === "warning" ? Theme.warning
                         : entry.tone === "success" ? Theme.success
                         : Theme.accentStroke
                }
                FontMetrics {
                    id: messageMetrics
                    font: messageText.font
                }
                Text {
                    id: timeText
                    anchors.left: toneDot.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.baseline: messageText.baseline
                    text: Qt.formatTime(entry.modelData.time, "HH:mm")
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                Text {
                    id: messageText
                    objectName: "activityMessageText_" + entry.index
                    anchors.left: timeText.right
                    anchors.right: entryAction.visible ? entryAction.left : parent.right
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(entry.modelData.text)
                    wrapMode: Text.Wrap
                    // Expanded: every line (a long reason, a path).
                    maximumLineCount: entry.expanded ? 100 : 2
                    elide: Text.ElideRight
                    color: entry.modelData.missed === true ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                HoverHandler {
                    id: entryHover
                    cursorShape: entry.cut || entry.expanded ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
                TapHandler {
                    objectName: "activityMessageExpand_" + entry.index
                    onTapped: entry.toggleExpanded()
                }
                PillButton {
                    id: entryAction
                    objectName: "activityMessageAction_" + entry.index
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: entry.hasAction
                    compact: true
                    variant: "ghost"
                    text: String(entry.modelData.actionText ?? "")
                    onClicked: {
                        const run = entry.modelData.action
                        root.close()
                        if (typeof run === "function")
                            run()
                    }
                }
            }
        }
    }
}
