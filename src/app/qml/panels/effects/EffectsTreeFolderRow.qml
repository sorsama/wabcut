pragma ComponentBehavior: Bound
import QtQuick

// A folder of the Effects panel's tree (UX rework; an EffectsTree row of rowType "folder"): a chevron, a folder or
// open-folder icon, the name and the item count (matches while searching). A click or Left/Right opens and closes it,
// a right-click opens the folder menu, F2 renames a custom bin or one of the user's presets bins in place (`renaming`).
// Effects dropped on a custom bin go into it; a preset of yours dropped on a presets bin moves there; a
// .wabcut-presets.json file dropped from Explorer is imported into it. The Audio Plug-ins folder shows its tools
// (PluginBrowser: grouping, rescan, settings, blocked plug-ins) under its row while it is open.
Item {
    id: row

    // EffectsTree roles.
    required property string rowKey
    required property int depth
    required property string title
    required property int count
    required property bool expanded
    required property string folderKind
    required property bool canHoldItems
    required property bool canHoldPresets

    /// The EffectsPanel: its view, selection, rename state and actions.
    property var panel: null

    readonly property bool selected: panel !== null && panel.currentKey === rowKey
    readonly property bool renaming: panel !== null && panel.renamingKey === rowKey
    readonly property bool topLevel: depth === 0
    readonly property bool dropTarget: panel !== null && panel.dropFolderKey === rowKey
    readonly property real indent: panel !== null ? depth * panel.indentStep : 0
    readonly property bool showsTools: expanded && folderKind === "plugins"

    objectName: "effectFolder_" + rowKey
    width: ListView.view ? ListView.view.width : 0
    height: header.height + (tools.status === Loader.Ready ? tools.height + Theme.space(2) : 0)

    // The list keeps the keyboard; the selected row is the accessible focus, so screen readers follow it as it moves.
    // (Qt 6.11's Accessible has no expanded state: the description says open or closed.)
    Accessible.role: Accessible.TreeItem
    Accessible.name: row.title
    Accessible.description: (row.expanded ? qsTr("Folder, open") : qsTr("Folder, closed")) + ", "
                            + (row.count === 1 ? qsTr("1 item") : qsTr("%1 items").arg(row.count))
    Accessible.selectable: true
    Accessible.selected: row.selected
    Accessible.focusable: true
    Accessible.focused: row.selected && !row.renaming && row.panel !== null && row.panel.treeFocused
    Accessible.onPressAction: row.panel.toggleFolder(row.rowKey)

    Item {
        id: header
        x: row.indent
        width: row.width - row.indent
        height: row.panel !== null ? row.panel.rowHeight : Theme.controlHeight

        Rectangle {
            id: background
            anchors.fill: parent
            radius: height / 2
            color: row.dropTarget ? Qt.alpha(Theme.accentFill, 0.22)
                 : row.selected ? Theme.hover
                 : hover.hovered ? Theme.raised : "transparent"
            border.width: row.dropTarget ? Theme.focusRingWidth : 0
            border.color: Theme.accentStroke

            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            FocusRing {
                targetRadius: background.radius
                visible: row.selected && !row.renaming && row.panel !== null && row.panel.showFocus
            }
        }

        Icon {
            id: chevron
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            name: row.expanded ? "chevron-down" : "chevron-right"
            size: Theme.iconXs
            color: Theme.textTertiary
        }
        Icon {
            id: folderIcon
            anchors.left: chevron.right
            anchors.leftMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            name: row.expanded ? "folder-open" : "folder"
            size: row.panel !== null ? row.panel.iconSize : Theme.iconSm
            color: row.dropTarget || row.selected ? Theme.accentStroke : Theme.textSecondary
        }
        Text {
            id: name
            visible: !row.renaming
            anchors.left: folderIcon.right
            anchors.leftMargin: Theme.space(2)
            anchors.right: countText.left
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            text: row.title
            elide: Text.ElideRight
            color: row.topLevel ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: row.panel !== null ? row.panel.textSize : Theme.textMd
            font.weight: row.topLevel ? Theme.weightLabel : Theme.weightBody
        }
        Text {
            id: countText
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            visible: !row.renaming
            text: row.count
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }

        // In-place rename (F2, the menu's Rename, a new bin): Enter keeps the name, Esc goes back, leaving it keeps it
        // (and the keyboard stays where the user went).
        Loader {
            id: renameLoader
            active: row.renaming
            anchors.left: folderIcon.right
            anchors.leftMargin: Theme.space(1)
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: FieldInput {
                id: field
                objectName: "effectsRenameField"
                implicitHeight: header.height - Theme.space(1)
                text: row.title
                Accessible.name: qsTr("Name of %1").arg(row.title)
                onAccepted: row.panel.commitRename(field.text, false)
                Keys.onEscapePressed: event => {
                    event.accepted = true
                    row.panel.cancelRename()
                }
                onActiveFocusChanged: {
                    if (!activeFocus && row.renaming)
                        row.panel.commitRename(field.text, true)
                }
            }
            onLoaded: {
                const field = item as FieldInput
                field.forceActiveFocus(Qt.OtherFocusReason)
                field.selectAll()
            }
        }

        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            id: leftTap
            acceptedButtons: Qt.LeftButton
            enabled: !row.renaming
            onTapped: {
                row.panel.pressRow(row.rowKey)
                if (leftTap.tapCount === 1) // a double-click does not close what its first click opened
                    row.panel.toggleFolder(row.rowKey)
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            enabled: !row.renaming
            onTapped: point => {
                row.panel.pressRow(row.rowKey)
                row.panel.openMenu(row.rowKey, header, point.position.x, point.position.y)
            }
        }

        // Drops: effects into a custom bin, a preset of yours into a presets bin, presets files from Explorer.
        DropArea {
            anchors.fill: parent
            onEntered: drag => {
                drag.accepted = row.panel.dropEnter(row.rowKey, drag)
            }
            onExited: row.panel.dropLeave(row.rowKey)
            onDropped: drop => row.panel.dropOn(row.rowKey, drop)
        }
    }

    // The Audio Plug-ins folder's tools, under its row while it is open.
    Loader {
        id: tools
        active: row.showsTools
        x: row.indent + Theme.space(3)
        y: header.height + Theme.space(1)
        width: row.width - x - Theme.space(2)
        sourceComponent: PluginBrowser {
            onStatus: message => row.panel.showStatus(message)
        }
    }
}
