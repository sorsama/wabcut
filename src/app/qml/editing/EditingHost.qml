pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// The dialogs and panels of Premiere's editing and sequence commands (P.2, E5), loaded on first use and opened by the
// Editing bridge's requests: Preferences (Media, Timeline), Sequence Settings, Frame Hold Options, Replace Footage,
// Merge Clips, Interpret Footage, Modify > Timecode. One element in Main.qml, so the shell carries no more than that
// hook. P1 round (#4): the History panel is a docked panel now (Main.qml); its Delete and Clear History ask here first.
Item {
    id: host
    objectName: "editingHost"

    /// Editing's messages go here (Main shows them as toasts).
    signal messageShown(string message)

    Loader {
        id: preferences
        objectName: "editingPreferencesLoader"
        active: false
        sourceComponent: EditingPreferencesDialog {}
    }
    Loader {
        id: sequenceSettings
        objectName: "sequenceSettingsLoader"
        active: false
        sourceComponent: SequenceSettingsDialog {}
    }
    Loader {
        id: frameHold
        objectName: "frameHoldOptionsLoader"
        active: false
        sourceComponent: FrameHoldOptionsDialog {}
    }
    Loader {
        id: fieldOptions
        objectName: "fieldOptionsLoader"
        active: false
        sourceComponent: FieldOptionsDialog {}
    }
    Loader {
        id: mergeClips
        objectName: "mergeClipsLoader"
        active: false
        sourceComponent: MergeClipsDialog {}
    }
    Loader {
        id: interpretFootage
        objectName: "interpretFootageLoader"
        active: false
        sourceComponent: InterpretFootageDialog {}
    }
    Loader {
        id: rawSourceSettings
        objectName: "rawSourceSettingsLoader"
        active: false
        sourceComponent: RawSourceSettingsDialog {}
    }
    Loader {
        id: timecode
        objectName: "timecodeLoader"
        active: false
        sourceComponent: TimecodeDialog {}
    }
    // P1 round (#4): History > Delete / Clear History cannot be undone: asked first.
    MenuConfirmDialog {
        id: historyConfirm
        objectName: "historyConfirmDialog"
    }
    // Replace Footage: the new file; the clip takes its name (Premiere's "Rename Clip to File Name").
    FileDialog {
        id: replaceFootageDialog
        objectName: "replaceFootageDialog"
        property string mediaKey: ""
        title: qsTr("Replace Footage")
        fileMode: FileDialog.OpenFile
        onAccepted: Editing.replaceFootage(replaceFootageDialog.mediaKey, selectedFile, true)
    }

    function openPreferences(page) {
        preferences.active = true
        const dialog = preferences.item as EditingPreferencesDialog
        dialog.openAt(page)
    }

    Connections {
        target: Editing

        function onPreferencesRequested(page) {
            host.openPreferences(page)
        }
        function onSequenceSettingsRequested(key) {
            sequenceSettings.active = true
            const dialog = sequenceSettings.item as SequenceSettingsDialog
            dialog.openFor(key)
        }
        function onFrameHoldOptionsRequested(key) {
            frameHold.active = true
            const dialog = frameHold.item as FrameHoldOptionsDialog
            dialog.openFor(key)
        }
        function onFieldOptionsRequested(key) {
            fieldOptions.active = true
            const dialog = fieldOptions.item as FieldOptionsDialog
            dialog.openFor(key)
        }
        function onMergeClipsRequested(keys) {
            mergeClips.active = true
            const dialog = mergeClips.item as MergeClipsDialog
            dialog.openFor(keys)
        }
        function onInterpretFootageRequested(keys) {
            interpretFootage.active = true
            const dialog = interpretFootage.item as InterpretFootageDialog
            dialog.openFor(keys)
        }
        function onRawSourceSettingsRequested(keys) {
            rawSourceSettings.active = true
            const dialog = rawSourceSettings.item as RawSourceSettingsDialog
            dialog.openFor(keys)
        }
        function onTimecodeRequested(keys) {
            timecode.active = true
            const dialog = timecode.item as TimecodeDialog
            dialog.openFor(keys)
        }
        function onReplaceFootageRequested(key) {
            replaceFootageDialog.mediaKey = key
            replaceFootageDialog.open()
        }
        function onHistoryEditRequested(action: string, index: int) {
            if (action === "clear") {
                historyConfirm.ask(qsTr("Clear the history?"),
                                   qsTr("Every undo step goes; the project stays as it is now. This cannot be undone."),
                                   qsTr("Clear History"), () => Editing.clearHistory())
                return
            }
            const steps = Editing.historySteps
            const label = index >= 0 && index < steps.length ? (steps[index].label ?? "") : ""
            const later = Math.max(0, steps.length - index - 1)
            historyConfirm.ask(qsTr("Delete “%1”?").arg(label),
                               later > 0 ? qsTr("This step and the %n step(s) after it are undone and go from the history. This cannot be undone.", "", later)
                                         : qsTr("This step is undone and goes from the history. This cannot be undone."),
                               qsTr("Delete"), () => Editing.deleteHistoryStep(index))
        }
        function onMessageChanged() {
            if (Editing.message !== "")
                host.messageShown(Editing.message)
        }
    }
}
