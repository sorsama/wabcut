pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Export Timeline… (0.1.1; Premiere's File > Export > Final Cut Pro XML / EDL / OTIO): writes a sequence as a
// timeline file other editors read, through the interchange.export command (ProjectFile.exportTimeline, on a worker).
// When it is written, `exported` hands the export report to Main, which shows it in ImportReportDialog.
// P.2: AAF and OMF for audio post (Premiere's File > Export > AAF / OMF), with their settings (AudioPostOptions).
// 0.2: several sequences at once - tick more than one in the Sequence menu and choose a folder: one file each, named
// after the sequence (ProjectFile.exportTimelines), and one combined report.
WabcutDialog {
    id: dialog
    objectName: "exportTimelineDialog"

    readonly property var formats: [
        { id: "fcp7-xml", label: qsTr("Premiere / Resolve XML"), suffix: "xml", filter: qsTr("Final Cut Pro 7 XML (*.xml)") },
        { id: "fcpxml", label: qsTr("FCPXML"), suffix: "fcpxml", filter: qsTr("FCPXML (*.fcpxml)") },
        { id: "otio", label: qsTr("OTIO"), suffix: "otio", filter: qsTr("OpenTimelineIO (*.otio)") },
        { id: "edl", label: qsTr("EDL"), suffix: "edl", filter: qsTr("CMX 3600 EDL (*.edl)") },
        { id: "aaf", label: qsTr("AAF"), suffix: "aaf", filter: qsTr("AAF (*.aaf)"), audioPost: true },
        { id: "omf", label: qsTr("OMF"), suffix: "omf", filter: qsTr("OMF (*.omf)"), audioPost: true }
    ]
    readonly property var format: formats[formatTabs.currentIndex] ?? formats[0]
    property int sequenceIndex: 0
    /// 0.2: the sequences to export (indices in ProjectController.sequenceNames); more than one writes a file each into
    /// `folder`.
    property var sequenceIndices: [0]
    readonly property bool several: sequenceIndices.length > 1
    property url folder: ""
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
        sequenceIndices = [sequenceIndex]
        folder = ProjectFile.folder
        file = suggestedFile()
        errorText = ""
        running = false
        open()
    }
    /// Ticks or unticks a sequence (at least one stays ticked).
    function toggleSequence(index) {
        const list = sequenceIndices.slice()
        const at = list.indexOf(index)
        if (at >= 0 && list.length > 1)
            list.splice(at, 1)
        else if (at < 0)
            list.push(index)
        list.sort((a, b) => a - b)
        sequenceIndices = list
        sequenceIndex = list[0]
        if (!several)
            file = suggestedFile()
    }
    function selectAllSequences() {
        const all = []
        for (let i = 0; i < ProjectController.sequenceNames.length; ++i)
            all.push(i)
        sequenceIndices = all
        sequenceIndex = 0
    }
    function exportNow() {
        errorText = ""
        const options = format.audioPost === true ? audioPostOptions.values() : ({})
        const started = several ? ProjectFile.exportTimelines(format.id, sequenceIndices, folder, options)
                                : ProjectFile.exportTimeline(format.id, sequenceIndex, file, options)
        if (started)
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
        text: dialog.several ? qsTr("Writes each sequence as a timeline file another editor opens, named after the sequence. The export report lists anything the format cannot carry.")
                             : qsTr("Writes the sequence as a timeline file another editor opens. The export report lists anything the format cannot carry.")
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
            text: dialog.several ? qsTr("%n sequence(s)", "", dialog.sequenceIndices.length)
                                 : ProjectController.sequenceNames[dialog.sequenceIndex] ?? ""
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
                        objectName: "exportTimelineSequence_" + index
                        text: modelData
                        iconName: "clapperboard"
                        checkable: true
                        checked: dialog.sequenceIndices.indexOf(index) >= 0
                        onTriggered: dialog.toggleSequence(index) // 0.2: tick several to export each
                    }
                    onObjectAdded: (index, object) => sequenceMenu.insertItem(index, object as ContextMenuItem)
                    onObjectRemoved: (index, object) => sequenceMenu.removeItem(object as ContextMenuItem)
                }
                ContextMenuSeparator {}
                ContextMenuItem {
                    objectName: "exportTimelineAllSequences"
                    text: qsTr("All sequences")
                    iconName: "layers"
                    onTriggered: dialog.selectAllSequences()
                }
            }
        }

        Label {
            visible: dialog.several
            text: qsTr("Folder")
        }
        RowLayout {
            visible: dialog.several
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                objectName: "exportTimelineFolder"
                text: dialog.folder.toString() === "" ? qsTr("Choose where to save them")
                                                      : dialog.folder.toString().replace(/^file:\/\/\/?/, "")
                elide: Text.ElideMiddle
                color: dialog.folder.toString() === "" ? Theme.textTertiary : Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            PillButton {
                compact: true
                text: qsTr("Choose…")
                Accessible.name: qsTr("Choose the folder")
                onClicked: folderDialog.open()
            }
        }

        Label {
            visible: !dialog.several
            text: qsTr("File")
        }
        RowLayout {
            visible: !dialog.several
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

    AudioPostOptions {
        id: audioPostOptions
        Layout.fillWidth: true
        visible: dialog.format.audioPost === true
        enabled: !dialog.running
        omf: dialog.format.id === "omf"
        sequenceName: ProjectController.sequenceNames[dialog.sequenceIndex] ?? ""
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

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder.toString() !== "" ? dialog.folder : ProjectFile.folder
        onAccepted: dialog.folder = selectedFolder
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
            enabled: !dialog.running && (dialog.several ? dialog.folder.toString() !== "" : dialog.file.toString() !== "")
            onClicked: dialog.exportNow()
        }
    ]
}
