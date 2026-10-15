pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Create Multi-Camera Source Sequence… (M5l, bridge `Multicam`): Premiere's dialog for the Library's selection. The
// angles in order (their names can be changed), how they line up (timecode, In points, Out points, clip markers or
// their sound) and the new sequence's name. Syncing by sound measures the angles first (A.5) and the report shows each
// angle's offset and how sure the match is; the other methods line up exactly.
WabcutDialog {
    id: dialog
    objectName: "multicamCreateDialog"

    property var media: []           // Library keys, in angle order
    property var items: []           // Multicam.describeMedia(media)
    property var names: []           // angle names typed in
    readonly property var syncIds: ["timecode", "inPoints", "outPoints", "markers", "audio"]
    readonly property var report: Multicam.report
    property bool started: false     // a Create of this opening ran
    readonly property bool done: started && report.ok === true

    function openFor(keys) {
        media = keys
        items = Multicam.describeMedia(keys)
        names = items.map(() => "")
        syncTabs.currentIndex = Math.max(0, syncIds.indexOf(Multicam.proposedSync(keys)))
        nameField.text = ""
        started = false
        open()
    }
    function create() {
        errorText.text = ""
        const error = Multicam.createSource(items.map(i => i.key), syncIds[syncTabs.currentIndex], nameField.text, names)
        if (error !== "")
            errorText.text = error
        else
            started = true
    }

    title: qsTr("Create Multi-Camera Source Sequence")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))
    onOpened: errorText.text = ""
    Connections {
        target: Multicam
        function onReportChanged() {
            if (dialog.started && Multicam.report.ok === false)
                errorText.text = Multicam.report.error ?? ""
        }
    }

    component Caption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    // ---- Before: the angles, the method, the name ----
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        visible: !dialog.done

        Caption {
            text: qsTr("Angles, in order (%n)", "", dialog.items.length)
        }
        Repeater {
            model: dialog.items
            delegate: RowLayout {
                id: angleRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    Layout.preferredWidth: Theme.space(6)
                    text: String(angleRow.index + 1)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
                Icon {
                    name: angleRow.modelData.kind === "audio" ? "music" : "film"
                    size: Theme.iconSm
                    color: Theme.textTertiary
                }
                FieldInput {
                    objectName: "multicamAngleName_" + (angleRow.index + 1)
                    Layout.fillWidth: true
                    placeholderText: angleRow.modelData.name
                    Accessible.name: qsTr("Name of angle %1").arg(angleRow.index + 1)
                    onTextEdited: {
                        const next = dialog.names.slice()
                        next[angleRow.index] = text
                        dialog.names = next
                    }
                }
            }
        }

        Caption {
            text: qsTr("Synchronize by")
        }
        PillTabs {
            id: syncTabs
            objectName: "multicamSyncMethod"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Timecode"), qsTr("In points"), qsTr("Out points"), qsTr("Clip markers"), qsTr("Audio")]
            Accessible.name: qsTr("Synchronize by")
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            text: syncTabs.currentIndex === 4
                  ? qsTr("The angles' sound is compared on this computer; the report says how sure each match is.")
                  : syncTabs.currentIndex === 0 ? qsTr("Each file's start timecode lines the angles up.")
                  : qsTr("The angles line up at their marks in the Source Monitor or at their first clip marker.")
        }

        Caption {
            text: qsTr("Sequence name")
        }
        FieldInput {
            id: nameField
            objectName: "multicamSequenceName"
            Layout.fillWidth: true
            Accessible.name: qsTr("Sequence name")
            placeholderText: dialog.items.length > 0 ? qsTr("%1 Multicam").arg(dialog.items[0].name) : ""
        }

        BeamProgress {
            objectName: "multicamProgress"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hairline * 3
            visible: Multicam.busy
            indeterminate: Multicam.progress < 0
            value: Math.max(0, Multicam.progress)
            active: Multicam.busy
            label: Multicam.phase
        }
        Text {
            id: errorText
            objectName: "multicamError"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // ---- After: the sync report ----
    ColumnLayout {
        objectName: "multicamReport"
        Layout.fillWidth: true
        spacing: Theme.space(2)
        visible: dialog.done

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            text: qsTr("“%1” is in the Library, synced by %2.").arg(dialog.report.name ?? "").arg((dialog.report.syncName ?? "").toLowerCase())
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: Theme.space(4)
            rowSpacing: Theme.space(1)
            Caption { text: qsTr("Angle") }
            Caption { text: qsTr("Starts at") }
            Caption { text: qsTr("Confidence") }
            Caption { text: qsTr("Note") }
            Repeater {
                model: (dialog.report.angles ?? []).length * 4
                delegate: Text {
                    required property int index
                    readonly property var row: (dialog.report.angles ?? [])[Math.floor(index / 4)] ?? ({})
                    readonly property int column: index % 4
                    Layout.fillWidth: column === 3
                    elide: Text.ElideRight
                    color: column === 3 && (row.problem ?? "") !== "" ? Theme.warning : column === 0 ? Theme.textPrimary : Theme.textSecondary
                    font.family: column === 1 || column === 2 ? Theme.fontMono : Theme.fontSans
                    font.pixelSize: Theme.textSm
                    text: column === 0 ? qsTr("%1 · %2").arg(row.angle ?? "").arg(row.name ?? "")
                          : column === 1 ? (row.offset ?? "")
                          : column === 2 ? (row.confidence ?? "")
                          : (row.problem ?? "") !== "" ? row.problem : (row.drift ?? "") !== "" ? qsTr("drift %1").arg(row.drift) : ""
                }
            }
        }
        Repeater {
            model: dialog.report.warnings ?? []
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                text: modelData
            }
        }
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: dialog.done ? qsTr("Close") : qsTr("Cancel")
            onClicked: {
                if (Multicam.busy)
                    Multicam.cancel()
                dialog.close()
            }
        },
        PillButton {
            objectName: "multicamCreateButton"
            visible: !dialog.done
            variant: "primary"
            enabled: !Multicam.busy && dialog.items.length > 0
            text: qsTr("Create")
            onClicked: dialog.create()
        }
    ]
}
