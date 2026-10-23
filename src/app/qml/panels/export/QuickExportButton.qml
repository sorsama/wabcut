pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Premiere's Quick Export (0.2): the header's one-click export of the current sequence. A popover picks the preset
// (built-in or the user's), shows what it writes, and the file name and location (default: next to the project, named
// after the sequence); Export queues it on ExportController like the Export workspace does, so it renders in the
// background while editing goes on.
// UX rework (stream L): it remembers the preset picked last (ExportController.lastQuickPreset) and starts in the folder
// exported to last; a marked sequence offers its range (Entire sequence / In to Out / Work Area, the last pick first);
// the presets are a keyboard list (Up/Down pick, Enter exports). When the export ends, Main shows a toast.
IconButton {
    id: root
    objectName: "quickExportButton"

    property string presetId: "match-source"
    property string output: ""
    /// The range the next export covers: "whole", "inOut" or "workArea" (set when the popover opens).
    property string range: "whole"
    readonly property var presetList: ExportController.presets.filter(p => p.category !== "hdr")
    readonly property var summary: popover.opened && ProjectController.revision >= 0 && ExportController.jobs.count >= 0
                                   && ExportController.autoEncoders
                                   ? ExportController.describe(ProjectController.sequenceId, presetId, root.overrides())
                                   : ({})
    /// The ranges this sequence has (always the whole sequence; In to Out with marks, a work area when set).
    readonly property var ranges: ["whole", "inOut", "workArea"].filter(r => r === "whole"
                                                                            || (root.summary.rangeAvailable ?? {})[r] === true)

    function overrides() {
        const o = root.range !== "whole" ? { range: root.range } : {}
        if (root.output !== "")
            o.output = root.output
        return o
    }
    function choosePreset(id) {
        root.presetId = id
        ExportController.lastQuickPreset = id
    }
    function exportNow() {
        if (ExportController.quickExport(root.presetId, root.output, root.overrides()) !== "") {
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
    // The last choice comes back (set once, so a pick is never overwritten).
    Component.onCompleted: presetId = ExportController.lastQuickPreset
    // Forgotten or changed elsewhere (the Export panel's Reset to defaults, a deleted user preset): the next Quick
    // Export starts from what is remembered now.
    Connections {
        target: ExportController
        function onExportMemoryChanged() {
            if (!popover.opened)
                root.presetId = ExportController.lastQuickPreset
        }
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
        onAboutToShow: {
            ExportController.probeEncoders()
            // The preset picked last (Match Source after the Export panel's Reset to defaults, or when a user preset
            // was deleted since).
            root.presetId = ExportController.lastQuickPreset
            const suggested = ExportController.suggestedRange("")
            root.range = suggested === "" ? "whole" : suggested
        }
        // The list takes the keys at once; its focus ring waits for a key (the pointer opened it, most often).
        onOpened: {
            presetView.forceActiveFocus(Qt.PopupFocusReason)
            presetView.keyboardUsed = false
        }

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
                activeFocusOnTab: true
                currentIndex: root.presetList.findIndex(p => p.id === root.presetId)
                highlightFollowsCurrentItem: false
                ScrollBar.vertical: ThinScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Quick Export preset")

                /// The focus ring shows once the keyboard is in use here (Tab in, or a key).
                property bool keyboardUsed: false
                onActiveFocusChanged: keyboardUsed = activeFocus

                // Up/Down pick the next preset, Enter exports with it. One handler for every key (a key-specific
                // handler like Keys.onUpPressed would take the key before onPressed, so the focus ring never showed).
                function step(delta: int) {
                    const next = Math.max(0, Math.min(count - 1, currentIndex + delta))
                    if (next < 0 || next >= root.presetList.length)
                        return
                    root.choosePreset(root.presetList[next].id)
                    positionViewAtIndex(next, ListView.Contain)
                }
                Keys.onPressed: event => {
                    keyboardUsed = true
                    if (event.key === Qt.Key_Up)
                        step(-1)
                    else if (event.key === Qt.Key_Down)
                        step(1)
                    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (root.summary.ok === true)
                            root.exportNow()
                    } else {
                        event.accepted = false
                        return
                    }
                    event.accepted = true
                }
                Keys.onShortcutOverride: event => event.accepted = [Qt.Key_Up, Qt.Key_Down, Qt.Key_Return, Qt.Key_Enter].includes(event.key)

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property bool current: modelData.id === root.presetId
                    width: presetView.width - Theme.space(2)
                    x: Theme.space(1)
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
                        onClicked: root.choosePreset(row.modelData.id)
                    }
                    FocusRing {
                        targetRadius: row.radius
                        visible: row.current && presetView.activeFocus && presetView.keyboardUsed
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

            // A marked sequence (or one with a work area): which part to export. The pick is remembered.
            FieldLabel {
                visible: rangeTabs.visible
                text: qsTr("Range")
            }
            PillTabs {
                id: rangeTabs
                objectName: "quickExportRange"
                width: parent.width
                visible: root.ranges.length > 1
                compact: true
                model: root.ranges.map(r => r === "inOut" ? qsTr("In to Out") : r === "workArea" ? qsTr("Work Area")
                                                                                                   : qsTr("Entire sequence"))
                currentIndex: Math.max(0, root.ranges.indexOf(root.range))
                Accessible.name: qsTr("Range")
                onActivated: index => {
                    root.range = root.ranges[index]
                    ExportController.lastRange = root.range
                    currentIndex = Qt.binding(() => Math.max(0, root.ranges.indexOf(root.range)))
                }
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
                    objectName: "quickExportBrowse"
                    compact: true
                    text: qsTr("Browse…")
                    tooltip: qsTr("Choose where to save the export")
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
                text: root.range === "inOut" ? qsTr("Export In to Out")
                      : root.range === "workArea" ? qsTr("Export Work Area") : qsTr("Export")
                enabled: root.summary.ok === true
                onClicked: root.exportNow()
            }
        }
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Quick Export to")
        fileMode: FileDialog.SaveFile
        // The folder exported to last, else next to the project (UX rework).
        currentFolder: ExportController.folderUrlOf(root.summary.output ?? "")
        onAccepted: root.output = selectedFile.toString()
    }
}
