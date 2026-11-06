pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Render and Replace (0.7, card 069; Clip > Render and Replace…): the selected nested-sequence clips are rendered to a file and
// play that file from then on, so a heavy nest scrubs and plays smoothly. The dialog lists the nests the selection plays and
// the stretch of each that is rendered (what the clips play plus the handles), asks for the format (an intermediate codec; ProRes
// 4444 keeps transparency), the handles (extra seconds either side, so a trim can still reach more picture) and the folder, then
// queues the renders in the export queue. It stays open while they run, with their progress and Stop; the clips are replaced as
// each file is done, one undo step each. Clip > Restore Unrendered plays the nest again.
WabcutDialog {
    id: dialog
    objectName: "renderReplaceDialog"
    title: qsTr("Render and Replace")

    /// RenderReplace.inspect(): { ok, reason, units: [{ name, clips, kind, usedText, rangeText, transparent }], preset, folder }.
    property var info: ({ ok: false, reason: "", units: [], preset: "", folder: "" })
    property string preset: ""
    property real handles: 1.0
    property string folder: ""
    /// A render began from this dialog: it closes itself when the last one ends (the toast says how it went).
    property bool started: false
    readonly property bool mayBeTransparent: (info.units ?? []).some(u => u.transparent === true)
    readonly property bool presetKeepsAlpha: RenderReplace.presets.some(p => p.id === dialog.preset && p.alpha === true)
    readonly property int shownUnits: 3

    function openDialog() {
        handles = RenderReplace.handleSeconds
        dialog.refresh()
        preset = info.preset ?? ""
        folder = info.folder ?? ""
        started = false
        RenderReplace.clearStatus()
        open()
    }
    function refresh() {
        info = RenderReplace.inspect(dialog.handles)
    }
    function begin() {
        if (RenderReplace.start(dialog.preset, dialog.handles, dialog.folder))
            dialog.started = true
    }

    Connections {
        target: RenderReplace
        function onChanged() {
            if (dialog.started && !RenderReplace.running) {
                dialog.started = false
                dialog.close()
            }
        }
    }

    Text {
        objectName: "renderReplaceIntro"
        Layout.fillWidth: true
        text: dialog.info.ok === true ? qsTr("The clips below play a nested sequence that is composited again at every frame. Rendering it once to a file makes playback smooth; the clips keep their place, trims, speed and effects. Clip > Restore Unrendered brings the nest back.")
                                      : (dialog.info.reason ?? "")
        color: dialog.info.ok === true ? Theme.textSecondary : Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: dialog.info.ok === true
        spacing: Theme.space(1)

        Repeater {
            model: (dialog.info.units ?? []).slice(0, dialog.shownUnits)
            delegate: ColumnLayout {
                id: unitRow
                required property var modelData
                required property int index
                objectName: "renderReplaceUnit_" + index
                Layout.fillWidth: true
                spacing: 0
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1, %2, plays %3, renders %4").arg(modelData.name).arg(modelData.kind).arg(modelData.usedText).arg(modelData.rangeText)
                Text {
                    Layout.fillWidth: true
                    //: Render and Replace: the nest's name, then what the clips of it carry
                    text: qsTr("%1  ·  %2").arg(unitRow.modelData.name).arg(unitRow.modelData.kind)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    //: Render and Replace: %1 = the stretch the clips play, %2 = the stretch rendered (handles added)
                    text: qsTr("Plays %1, renders %2").arg(unitRow.modelData.usedText).arg(unitRow.modelData.rangeText)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
            }
        }
        Text {
            objectName: "renderReplaceMore"
            visible: (dialog.info.units ?? []).length > dialog.shownUnits
            Layout.fillWidth: true
            text: qsTr("and %n more nest(s)", "", Math.max(0, (dialog.info.units ?? []).length - dialog.shownUnits))
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: dialog.info.ok === true
        spacing: Theme.space(1)

        Text {
            text: qsTr("Format")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Repeater {
            model: RenderReplace.presets
            delegate: AbstractButton {
                id: option
                required property var modelData
                readonly property bool selected: dialog.preset === modelData.id
                objectName: "renderReplacePreset_" + modelData.id
                Layout.fillWidth: true
                topPadding: Theme.space(1.5)
                bottomPadding: Theme.space(1.5)
                leftPadding: Theme.space(3)
                rightPadding: Theme.space(3)
                implicitHeight: implicitContentHeight + topPadding + bottomPadding
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                enabled: !RenderReplace.running
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name
                Accessible.description: modelData.note
                Accessible.checked: selected
                onClicked: dialog.preset = modelData.id

                background: Rectangle {
                    radius: Theme.radiusField
                    color: option.selected ? Theme.accentWash : option.hovered ? Theme.hover : "transparent"
                    border.width: option.selected ? Theme.hairline : 0
                    border.color: Theme.accentStroke
                }
                contentItem: RowLayout {
                    spacing: Theme.space(2)
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Theme.space(0.25)
                        implicitWidth: Theme.iconSm
                        implicitHeight: Theme.iconSm
                        radius: width / 2
                        color: "transparent"
                        border.width: Theme.focusRingWidth
                        border.color: option.selected ? Theme.accentStroke : Theme.textTertiary
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width / 2
                            height: width
                            radius: width / 2
                            color: Theme.accentStroke
                            visible: option.selected
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: option.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: option.selected ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            Layout.fillWidth: true
                            text: option.modelData.note
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.Wrap
                        }
                    }
                }
                FocusRing {
                    anchors.fill: parent
                    targetRadius: Theme.radiusField
                    visible: option.visualFocus
                }
                HoverHandler {
                    enabled: option.enabled
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
        Text {
            objectName: "renderReplaceAlphaWarning"
            visible: dialog.mayBeTransparent && !dialog.presetKeepsAlpha
            Layout.fillWidth: true
            text: qsTr("A nest above shows transparency (a gap or a graphic on its lowest track). This format fills it with black: choose ProRes 4444 to keep it.")
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: dialog.info.ok === true
        spacing: Theme.space(2)
        Text {
            text: qsTr("Handles")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        NumberField {
            objectName: "renderReplaceHandles"
            implicitWidth: Theme.space(22)
            from: 0
            to: 60
            stepSize: 0.5
            decimals: 1
            suffix: qsTr(" s")
            value: dialog.handles
            enabled: !RenderReplace.running
            Accessible.name: qsTr("Handles, seconds on each side")
            onValueModified: v => {
                dialog.handles = v
                dialog.refresh()
            }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("rendered before and after what the clips play, so a later trim still has picture")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: dialog.info.ok === true
        spacing: Theme.space(2)
        Text {
            text: qsTr("Save to")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Text {
            objectName: "renderReplaceFolder"
            Layout.fillWidth: true
            text: dialog.folder
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            elide: Text.ElideMiddle
            Accessible.name: qsTr("Folder for the rendered files: %1").arg(dialog.folder)
        }
        PillButton {
            objectName: "renderReplaceChooseFolder"
            compact: true
            text: qsTr("Folder…")
            enabled: !RenderReplace.running
            tooltip: qsTr("Where the rendered files are saved")
            onClicked: folderDialog.open()
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Save rendered files in")
        onAccepted: dialog.folder = RenderReplace.localPath(folderDialog.selectedFolder)
    }

    ColumnLayout {
        objectName: "renderReplaceProgress"
        Layout.fillWidth: true
        visible: RenderReplace.running || RenderReplace.status !== ""
        spacing: Theme.space(1)
        BeamProgress {
            Layout.fillWidth: true
            visible: RenderReplace.running
            value: RenderReplace.progress
            label: RenderReplace.status
        }
        Text {
            objectName: "renderReplaceStatus"
            Layout.fillWidth: true
            text: RenderReplace.status
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
    }

    footerData: [
        PillButton {
            objectName: "renderReplaceStop"
            visible: RenderReplace.running
            text: qsTr("Stop")
            tooltip: qsTr("Stop rendering: the clips keep playing the nest")
            onClicked: RenderReplace.cancel()
        },
        PillButton {
            objectName: "renderReplaceCancel"
            text: RenderReplace.running ? qsTr("Hide") : qsTr("Cancel")
            tooltip: RenderReplace.running ? qsTr("Close this window: the renders go on in the export queue") : ""
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "renderReplaceStart"
            text: qsTr("Render and Replace")
            variant: "primary"
            enabled: dialog.info.ok === true && !RenderReplace.running && dialog.preset !== ""
            onClicked: dialog.begin()
        }
    ]
}
