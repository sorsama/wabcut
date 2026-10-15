pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The version history (M5m, bridge `History`; C.9): a named "Save version", the versions newest first (saved,
// autosaves, before an AI edit, before a restore, named ones; a double-click renames one), and per version: restore
// (one undo step: the project as it is now is kept as a version first), restore one of its sequences as a new
// sequence, export it as a project file, compare it with now or with another version. A comparison shows its changes
// as lines (VersionCompareView) and the timeline highlights the changed ranges of the current sequence.
Item {
    id: root
    objectName: "reviewVersionsPage"
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Versions")

    /// The version picked as the first side of "Compare with…" (0: none).
    property int compareFrom: 0
    /// The version being renamed in place (0: none): a double-click on its row, or More > Rename.
    property int renameVersion: 0
    /// The version the actions popover is for.
    property var actionVersion: ({})

    function kindIcon(kind) {
        return kind === "save" ? "save" : kind === "auto" ? "clock" : kind === "agent" ? "sparkles"
             : kind === "restore" ? "rotate-ccw" : "bookmark"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !History.available
            illustration: "timeline"
            text: History.reason
        }

        // Save a named version.
        RowLayout {
            Layout.fillWidth: true
            visible: History.available
            spacing: Theme.space(2)
            FieldInput {
                id: versionName
                objectName: "historyVersionName"
                Layout.fillWidth: true
                placeholderText: qsTr("Name this version, e.g. Client cut v2")
                Accessible.name: qsTr("Version name")
                onAccepted: saveButton.clicked()
            }
            PillButton {
                id: saveButton
                objectName: "historySaveVersion"
                variant: "primary"
                compact: true
                iconName: "bookmark"
                text: qsTr("Save version")
                enabled: versionName.text.trim() !== ""
                onClicked: {
                    if (History.saveVersion(versionName.text) === "")
                        versionName.text = ""
                }
            }
        }

        // Picking the second side of a comparison.
        Rectangle {
            Layout.fillWidth: true
            visible: root.compareFrom > 0
            implicitHeight: pickRow.implicitHeight + Theme.space(2)
            radius: Theme.radiusField
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            RowLayout {
                id: pickRow
                anchors.fill: parent
                anchors.margins: Theme.space(1)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Choose the version to compare version %1 with").arg(root.compareFrom)
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    compact: true
                    variant: "ghost"
                    text: qsTr("Cancel")
                    onClicked: root.compareFrom = 0
                }
            }
        }

        ListView {
            id: list
            objectName: "historyVersionsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: History.comparison.from !== undefined ? 1 : 2
            visible: History.available
            clip: true
            spacing: Theme.space(1)
            model: History.versions
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property int versionId: modelData.id
                readonly property bool picked: root.compareFrom === versionId
                readonly property bool renaming: root.renameVersion === versionId
                readonly property bool pickable: root.compareFrom > 0 && !picked

                /// The second version of "Compare with another version…".
                function pickForCompare() {
                    History.compare(root.compareFrom, row.versionId)
                    root.compareFrom = 0
                }

                objectName: "historyVersion_" + versionId
                width: ListView.view.width
                implicitHeight: rowLayout.implicitHeight + Theme.space(2.5)
                radius: Theme.radiusField
                color: rowHover.hovered ? Theme.hover : "transparent"
                border.width: row.picked ? Theme.selectionWidth : 0
                border.color: Theme.accentStroke

                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, %2").arg(modelData.title).arg(modelData.createdText)
                Accessible.onPressAction: {
                    if (row.pickable)
                        row.pickForCompare()
                }

                HoverHandler {
                    id: rowHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        if (row.pickable)
                            row.pickForCompare()
                    }
                    onDoubleTapped: root.renameVersion = row.versionId
                }

                RowLayout {
                    id: rowLayout
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(1)
                    spacing: Theme.space(2)

                    Icon {
                        name: root.kindIcon(row.modelData.kind)
                        size: Theme.iconSm
                        color: row.modelData.named ? Theme.accentStroke : Theme.textTertiary
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            visible: !row.renaming
                            text: row.modelData.title + (row.modelData.latest ? "  ·  " + qsTr("latest") : "")
                            elide: Text.ElideRight
                            color: row.modelData.named ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: row.modelData.named ? Font.DemiBold : Font.Normal
                        }
                        FieldInput {
                            objectName: "historyRename_" + row.versionId
                            Layout.fillWidth: true
                            visible: row.renaming
                            text: row.modelData.label
                            placeholderText: qsTr("Version name (empty: unnamed)")
                            Accessible.name: qsTr("Version name")
                            onVisibleChanged: {
                                if (visible) {
                                    forceActiveFocus()
                                    selectAll()
                                }
                            }
                            onAccepted: {
                                History.name(row.versionId, text)
                                root.renameVersion = 0
                            }
                            Keys.onEscapePressed: root.renameVersion = 0
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 · %2 · %n clip(s) · %3", "", row.modelData.clips)
                                  .arg(row.modelData.createdText).arg(row.modelData.kindText).arg(row.modelData.sizeText)
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    // While choosing the second version of a comparison: this one (the row's click does the same).
                    IconButton {
                        objectName: "historyComparePick_" + row.versionId
                        visible: row.pickable
                        compact: true
                        iconName: "check"
                        tooltip: qsTr("Compare version %1 with this one").arg(root.compareFrom)
                        onClicked: row.pickForCompare()
                    }
                    IconButton {
                        objectName: "historyCompareNow_" + row.versionId
                        compact: true
                        iconName: "arrow-left-right"
                        tooltip: qsTr("Compare with now")
                        onClicked: History.compare(row.versionId, "current")
                    }
                    IconButton {
                        id: moreButton
                        objectName: "historyActions_" + row.versionId
                        compact: true
                        iconName: "ellipsis"
                        tooltip: qsTr("More")
                        onClicked: {
                            root.actionVersion = row.modelData
                            actions.parent = moreButton
                            actions.open()
                        }
                    }
                }
            }
        }

        // The comparison.
        VersionCompareView {
            objectName: "historyComparison"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 1
            visible: History.available && History.comparison.from !== undefined
            comparison: History.comparison
        }

        Text {
            Layout.fillWidth: true
            visible: History.available
            text: History.diskText
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }

    // A version's actions.
    Popover {
        id: actions
        objectName: "historyActionsPopover"
        // Rename… opens the name field once the popover has closed and given focus back, so the field keeps it.
        property int pendingRename: 0
        onClosed: {
            if (actions.pendingRename > 0)
                root.renameVersion = actions.pendingRename
            actions.pendingRename = 0
        }
        title: root.actionVersion.title ?? ""
        x: -width + Theme.space(8)
        y: Theme.space(8)
        transformOrigin: Item.TopRight

        Column {
            spacing: Theme.space(1)
            width: Theme.space(70)

            PillButton {
                objectName: "historyRestore"
                width: parent.width
                variant: "ghost"
                iconName: "rotate-ccw"
                text: qsTr("Restore this version")
                tooltip: qsTr("Go back to it (one undo step; the project as it is now is kept as a version)")
                onClicked: {
                    actions.close()
                    History.restore(root.actionVersion.id)
                }
            }
            Text {
                width: parent.width
                text: qsTr("Restore a sequence as a new one")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Repeater {
                model: actions.opened ? History.sequencesOf(root.actionVersion.id ?? 0) : []
                delegate: PillButton {
                    required property var modelData
                    width: parent.width
                    variant: "ghost"
                    compact: true
                    iconName: "plus"
                    text: modelData.name
                    onClicked: {
                        actions.close()
                        History.restoreSequence(root.actionVersion.id, modelData.key, "")
                    }
                }
            }
            PillButton {
                objectName: "historyRenameAction"
                width: parent.width
                variant: "ghost"
                iconName: "pencil"
                text: qsTr("Rename…")
                tooltip: qsTr("Name this version (also: double-click it)")
                onClicked: {
                    actions.pendingRename = root.actionVersion.id
                    actions.close()
                }
            }
            PillButton {
                objectName: "historyCompareWith"
                width: parent.width
                variant: "ghost"
                iconName: "arrow-left-right"
                text: qsTr("Compare with another version…")
                onClicked: {
                    actions.close()
                    root.compareFrom = root.actionVersion.id
                }
            }
            PillButton {
                objectName: "historyExport"
                width: parent.width
                variant: "ghost"
                iconName: "file-down"
                text: qsTr("Export as a project…")
                onClicked: {
                    actions.close()
                    exportDialog.versionId = root.actionVersion.id
                    exportDialog.open()
                }
            }
        }
    }

    FileDialog {
        id: exportDialog
        property int versionId: 0
        title: qsTr("Export version as project")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut"
        nameFilters: [qsTr("Wabcut projects (*.wabcut)")]
        currentFolder: ProjectFile.folder
        onAccepted: History.exportVersion(versionId, selectedFile, true)
    }
}
