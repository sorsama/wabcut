pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Export Timeline… (0.1.1; Premiere's File > Export > Final Cut Pro XML / EDL / OTIO): writes a sequence as a
// timeline file other editors read, through the interchange.export command (ProjectFile.exportTimeline, on a worker).
// When it is written, `exported` hands the export report to Main, which shows it in ImportReportDialog.
WabcutDialog {
    id: dialog
    objectName: "exportTimelineDialog"

    readonly property var formats: [
        { id: "fcp7-xml", label: qsTr("Premiere / Resolve XML"), suffix: "xml", filter: qsTr("Final Cut Pro 7 XML (*.xml)") },
        { id: "fcpxml", label: qsTr("FCPXML"), suffix: "fcpxml", filter: qsTr("FCPXML (*.fcpxml)") },
        { id: "otio", label: qsTr("OTIO"), suffix: "otio", filter: qsTr("OpenTimelineIO (*.otio)") },
        { id: "edl", label: qsTr("EDL"), suffix: "edl", filter: qsTr("CMX 3600 EDL (*.edl)") }
    ]
    readonly property var format: formats[formatTabs.currentIndex] ?? formats[0]
    property int sequenceIndex: 0
    property url file: ""
    property bool running: false
    property string errorText: ""

    /// The export report (ProjectFile.timelineExported) once the file is written.
    signal exported(var report)

    function withSuffix(path) {
        return path === "" ? "" : path.replace(/\.[^.\/\\]*$/, "") + "." + format.suffix
    }
    function suggestedFile() {
        const folder = ProjectFile.folder.toString()
        if (folder === "")
            return ""
        const name = String(ProjectController.sequenceNames[sequenceIndex] ?? qsTr("Sequence")).replace(/[<>:"\/\\|?*]/g, "_")
        return folder + "/" + name + "." + format.suffix
    }
    function openForCurrent() {
        sequenceIndex = Math.max(0, ProjectController.currentSequenceIndex)
        file = suggestedFile()
        errorText = ""
        running = false
        open()
    }
    function exportNow() {
        errorText = ""
        if (ProjectFile.exportTimeline(format.id, sequenceIndex, file))
            running = true
        else
            errorText = ProjectFile.lastError
    }

    title: qsTr("Export Timeline")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Connections {
        target: ProjectFile
        function onTimelineExported(report) {
            if (!dialog.running)
                return
            dialog.running = false
            if (report.ok) {
                dialog.close()
                dialog.exported(report)
            } else {
                dialog.errorText = report.error
            }
        }
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("Writes the sequence as a timeline file another editor opens. The export report lists anything the format cannot carry.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(2)
        enabled: !dialog.running

        Label { text: qsTr("Format") }
        PillTabs {
            id: formatTabs
            objectName: "exportTimelineFormat"
            Layout.fillWidth: true
            compact: true
            model: dialog.formats.map(f => f.label)
            onActivated: dialog.file = dialog.withSuffix(dialog.file.toString())
        }

        Label { text: qsTr("Sequence") }
        PillButton {
            id: sequenceButton
            objectName: "exportTimelineSequence"
            Layout.maximumWidth: Theme.space(80)
            compact: true
            variant: "secondary"
            text: ProjectController.sequenceNames[dialog.sequenceIndex] ?? ""
            iconName: "chevron-down"
            LayoutMirroring.enabled: true    // label first, chevron last
            LayoutMirroring.childrenInherit: true
            Accessible.name: qsTr("Sequence: %1").arg(text)
            onClicked: sequenceMenu.popup(sequenceButton, 0, sequenceButton.height + Theme.space(1))

            ContextMenu {
                id: sequenceMenu

                Instantiator {
                    model: ProjectController.sequenceNames
                    delegate: ContextMenuItem {
                        required property string modelData
                        required property int index
                        text: modelData
                        iconName: "clapperboard"
                        checkable: true
                        checked: index === dialog.sequenceIndex
                        onTriggered: dialog.sequenceIndex = index
                    }
                    onObjectAdded: (index, object) => sequenceMenu.insertItem(index, object as ContextMenuItem)
                    onObjectRemoved: (index, object) => sequenceMenu.removeItem(object as ContextMenuItem)
                }
            }
        }

        Label { text: qsTr("File") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                objectName: "exportTimelineFile"
                text: dialog.file.toString() === "" ? qsTr("Choose where to save it")
                                                    : dialog.file.toString().replace(/^file:\/\/\/?/, "")
                elide: Text.ElideMiddle
                color: dialog.file.toString() === "" ? Theme.textTertiary : Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            PillButton {
                compact: true
                text: qsTr("Choose…")
                onClicked: fileDialog.open()
            }
        }
    }

    Text {
        objectName: "exportTimelineError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.running ? qsTr("Exporting…") : dialog.errorText
        color: dialog.running ? Theme.textSecondary : Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    FileDialog {
        id: fileDialog
        fileMode: FileDialog.SaveFile
        nameFilters: [dialog.format.filter]
        defaultSuffix: dialog.format.suffix
        currentFolder: ProjectFile.folder
        onAccepted: dialog.file = selectedFile
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "exportTimelineOk"
            variant: "primary"
            text: qsTr("Export")
            enabled: !dialog.running && dialog.file.toString() !== ""
            onClicked: dialog.exportNow()
        }
    ]
}
