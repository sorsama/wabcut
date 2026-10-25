pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Quit guard (0.4): closing the main window (its ×, File > Exit, Alt+F4, the taskbar) while exports run, wait or are
// paused, or while Render In to Out runs, asks first (Main.readyToClose). The body says what is going on, live:
// the current export and its percentage, what waits, what is paused or held for Start Queue, Render In to Out, then what
// is lost (unfinished exports) and what is kept (rendered previews).
//   Cancel Exports and Quit   (danger; "Stop Rendering and Quit" when no export is involved) cancelAndQuitChosen()
//   Quit When Done            (only while something runs that ends on its own: `canWait`)   quitWhenDoneChosen()
//   Keep Wabcut Open          (primary, focused; Enter; Esc too)                            keepOpenChosen()
// Each button closes the dialog, then emits. When the work ends while it asks, Main closes it without an answer
// (settle()) and goes on with the close; until then it says so ("Quit Wabcut?", "The work in progress is done.") instead
// of naming work that no longer runs. Premiere: Media Encoder asks before it quits with items left in its queue.
WabcutDialog {
    id: dialog
    objectName: "backgroundWorkCloseDialog"

    readonly property ExportJobModel jobs: ExportController.jobs
    /// Exports that run without the user (running or queued to run).
    readonly property int running: dialog.jobs ? dialog.jobs.runningCount : 0
    /// Any unfinished export: running, queued, paused or held.
    readonly property bool exporting: dialog.jobs !== null && dialog.jobs.activeCount > 0
    readonly property bool rendering: MediaReadiness.rendering
    /// Quit When Done is offered: something runs that ends on its own.
    readonly property bool canWait: dialog.running > 0 || dialog.rendering
    /// The body (one paragraph, only the lines that apply).
    readonly property string message: {
        const j = dialog.jobs
        const lines = []
        if (j) {
            const percent = Math.round(Math.max(0, j.currentProgress) * 100)
            if (j.currentState === "running")
                //: Quit guard: the export running now; %1 its file name, %2 its percentage
                lines.push(qsTr("“%1” is exporting (%2 %).").arg(j.currentTitle).arg(percent))
            else if (j.currentState === "paused")
                lines.push(qsTr("“%1” is paused at %2 %.").arg(j.currentTitle).arg(percent))
            // English singular and plural spelled out: the pill and this text are read at a glance. Waiting: the queued
            // exports - they run on their own (runningCount), or wait behind a paused one (in activeCount alone).
            const waiting = j.activeCount - j.pausedCount - j.heldCount - (j.currentState === "running" ? 1 : 0)
            const more = j.currentState === "running" || j.currentState === "paused"
            if (waiting > 0)
                lines.push(more ? (waiting === 1 ? qsTr("1 more export waits to start.")
                                                 : qsTr("%1 more exports wait to start.").arg(waiting))
                                : (waiting === 1 ? qsTr("1 export waits to start.")
                                                 : qsTr("%1 exports wait to start.").arg(waiting)))
            const paused = j.pausedCount - (j.currentState === "paused" ? 1 : 0)
            if (paused > 0)
                lines.push(j.currentState === "paused"
                           ? (paused === 1 ? qsTr("1 more export is paused.") : qsTr("%1 more exports are paused.").arg(paused))
                           : (paused === 1 ? qsTr("1 export is paused.") : qsTr("%1 exports are paused.").arg(paused)))
            if (j.heldCount > 0)
                lines.push(j.heldCount === 1 ? qsTr("1 export waits for Start Queue.")
                                             : qsTr("%1 exports wait for Start Queue.").arg(j.heldCount))
        }
        if (dialog.rendering)
            lines.push(MediaReadiness.renderProgress >= 0
                       ? qsTr("Render In to Out is at %1 %.").arg(Math.round(MediaReadiness.renderProgress * 100))
                       : qsTr("Render In to Out is running."))
        if (dialog.exporting)
            lines.push(qsTr("Exports that have not finished are lost when Wabcut closes."))
        if (dialog.rendering)
            lines.push(qsTr("Previews rendered so far are kept."))
        // The work ended while it asked (Main closes it and goes on with the close).
        if (lines.length === 0)
            lines.push(qsTr("The work in progress is done."))
        return lines.join(" ")
    }
    /// The work ended while it asked: closes without an answer (Main goes on with the close).
    function settle() {
        dialog.answered = true
        dialog.close()
    }

    signal keepOpenChosen()
    signal quitWhenDoneChosen()
    signal cancelAndQuitChosen()

    // A button answered (Esc and a click outside are "Keep Wabcut Open").
    property bool answered: false
    function choose(which: string) {
        dialog.answered = true
        dialog.close()
        if (which === "wait")
            dialog.quitWhenDoneChosen()
        else if (which === "cancel")
            dialog.cancelAndQuitChosen()
        else
            dialog.keepOpenChosen()
    }
    onAboutToShow: dialog.answered = false
    onOpened: keepOpenButton.forceActiveFocus()
    onClosed: {
        if (!dialog.answered) {
            dialog.answered = true
            dialog.keepOpenChosen()
        }
    }

    title: dialog.running > 0 ? qsTr("Quit while exporting?")
         : dialog.exporting ? qsTr("Quit with exports in the queue?")
         : dialog.rendering ? qsTr("Quit while rendering?")
         : qsTr("Quit Wabcut?")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(160), Theme.space(160))

    Text {
        objectName: "backgroundWorkText"
        Layout.fillWidth: true
        text: dialog.message
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        Accessible.role: Accessible.StaticText
        Accessible.name: dialog.message
    }

    footerData: [
        PillButton {
            objectName: "backgroundWorkCancelAndQuit"
            variant: "danger"
            text: dialog.exporting ? qsTr("Cancel Exports and Quit")
                : dialog.rendering ? qsTr("Stop Rendering and Quit") : qsTr("Quit")
            onClicked: dialog.choose("cancel")
        },
        PillButton {
            objectName: "backgroundWorkQuitWhenDone"
            visible: dialog.canWait
            variant: "ghost" // an outline: a raised pill would not show on the raised card
            text: qsTr("Quit When Done")
            tooltip: qsTr("Wabcut closes by itself once the work in progress is done")
            onClicked: dialog.choose("wait")
        },
        PillButton {
            id: keepOpenButton
            objectName: "backgroundWorkKeepOpen"
            variant: "primary"
            text: qsTr("Keep Wabcut Open")
            onClicked: dialog.choose("keep")
            Keys.onReturnPressed: dialog.choose("keep")
            Keys.onEnterPressed: dialog.choose("keep")
        }
    ]
}
