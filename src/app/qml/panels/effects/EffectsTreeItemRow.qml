pragma ComponentBehavior: Bound
import QtQuick

// An effect, transition, preset or audio plug-in in the Effects panel's tree (UX rework; an EffectsTree row of rowType
// "item", the List and Compact views): its category icon and name, with its category while searching or when it is a
// copy in Favorites or a custom bin, a CLAP / VST3 tag on plug-ins, DEFAULT on the default transitions and a bookmark on
// favourites. The description is its tooltip. A double-click (or Enter) applies it to every selected clip; drag it onto
// the timeline, Effect Controls, a mixer strip or a custom bin; right-click for its menu. A plug-in with a problem is
// dimmed and cannot be dragged or applied.
Item {
    id: row

    // EffectsTree roles.
    required property string rowKey
    required property string parentKey
    required property int depth
    required property string itemId
    required property string name
    required property string category
    required property string description
    required property string icon
    required property string kind
    required property string format
    required property string problem
    required property bool plugin
    required property bool favorite
    required property bool home

    /// The EffectsPanel: its view, selection and actions.
    property var panel: null

    readonly property bool selected: panel !== null && panel.currentKey === rowKey
    readonly property bool usable: problem === ""
    readonly property bool compactView: panel !== null && panel.view === "compact"
    readonly property bool isDefault: (kind === "videoTransition" || kind === "audioTransition") && panel !== null
                                      && panel.defaultTransitions.includes(itemId)
    readonly property real indent: panel !== null ? depth * panel.indentStep : 0
    readonly property string tip: problem !== "" ? problem : description
    /// The hover tooltip: the description (or the problem), after the whole name when the row cuts it short.
    readonly property string tooltipText: label.truncated ? (tip !== "" ? name + " · " + tip : name) : tip

    // Its own place keeps the plain name (tests and the a11y audit find it); copies in Favorites or a custom bin say
    // where they are.
    objectName: home ? (plugin ? "pluginItem_" : "effectItem_") + itemId : "effectItem_" + parentKey + "_" + itemId
    width: ListView.view ? ListView.view.width : 0
    height: panel !== null ? panel.rowHeight : Theme.controlHeight
    opacity: usable ? 1 : Theme.disabledOpacity + 0.2

    // The list keeps the keyboard; the selected row is the accessible focus, so screen readers follow it as it moves.
    Accessible.role: Accessible.TreeItem
    Accessible.name: row.plugin ? qsTr("%1, audio plug-in").arg(row.name) : row.name
    Accessible.description: row.tip
    Accessible.selectable: true
    Accessible.selected: row.selected
    Accessible.focusable: true
    Accessible.focused: row.selected && row.panel !== null && row.panel.treeFocused
    Accessible.onPressAction: row.panel.activate(row.rowKey)

    Item {
        id: content
        x: row.indent
        width: row.width - row.indent
        height: row.height

        Rectangle {
            id: background
            anchors.fill: parent
            radius: height / 2
            color: row.selected ? Theme.hover : hover.hovered ? Theme.raised : "transparent"

            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            FocusRing {
                targetRadius: background.radius
                visible: row.selected && row.panel !== null && row.panel.showFocus
            }
        }

        Icon {
            id: itemIcon
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(row.compactView ? 2 : 3)
            anchors.verticalCenter: parent.verticalCenter
            name: row.icon
            size: row.panel !== null ? row.panel.iconSize : Theme.iconSm
            color: row.selected || hover.hovered || drag.dragging ? Theme.accentStroke : Theme.textTertiary
        }
        Text {
            id: label
            anchors.left: itemIcon.right
            anchors.leftMargin: Theme.space(2)
            anchors.right: categoryText.visible ? categoryText.left : meta.left
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            text: row.name
            elide: Text.ElideRight
            color: row.usable ? Theme.textPrimary : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: row.panel !== null ? row.panel.textSize : Theme.textMd
            font.weight: Theme.weightBody
        }
        TextMetrics {
            id: nameMetrics
            font.family: Theme.fontSans
            font.pixelSize: row.panel !== null ? row.panel.textSize : Theme.textMd
            font.weight: Theme.weightBody
            text: row.name
        }
        // The category while searching or on a copy (the folder says it otherwise), in the room the name leaves.
        Text {
            id: categoryText
            readonly property bool wanted: text !== "" && row.panel !== null && (row.panel.searching || !row.home)
            readonly property real room: meta.x - (meta.width > 0 ? Theme.space(1.5) : 0) - itemIcon.x - itemIcon.width
                                         - Theme.space(2) - nameMetrics.advanceWidth - Theme.space(3)
            anchors.right: meta.left
            anchors.rightMargin: meta.width > 0 ? Theme.space(1.5) : 0
            anchors.verticalCenter: parent.verticalCenter
            visible: wanted && room >= Theme.space(10)
            width: Math.min(implicitWidth, room)
            text: row.category
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Row {
            id: meta
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(1.5)

            MicroTag {
                visible: row.isDefault
                text: qsTr("Default")
            }
            MicroTag {
                visible: row.plugin && row.format !== ""
                text: row.format
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.favorite && row.parentKey !== "favorites"
                name: "bookmark"
                size: Theme.iconXs
                color: Theme.accentStroke
            }
        }

        HoverHandler {
            id: hover
            cursorShape: row.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        // Click, double-click, right-click and the drag.
        EffectsDragSource {
            id: drag
            anchors.fill: parent
            itemId: row.itemId
            imageItem: content
            dragEnabled: row.usable && row.renamingNothing
            onTapped: row.panel.pressRow(row.rowKey)
            onDoubleTapped: row.panel.activate(row.rowKey)
            onMenuRequested: (x, y) => {
                row.panel.pressRow(row.rowKey)
                row.panel.openMenu(row.rowKey, content, x, y)
            }
        }

        // A drop on an item goes to its folder (a custom bin or a presets bin).
        DropArea {
            anchors.fill: parent
            onEntered: dragEvent => {
                dragEvent.accepted = row.panel.dropEnter(row.rowKey, dragEvent)
            }
            onExited: row.panel.dropLeave(row.rowKey)
            onDropped: dropEvent => row.panel.dropOn(row.rowKey, dropEvent)
        }
    }

    Loader {
        active: hover.hovered && row.tooltipText !== "" && !drag.dragging
        sourceComponent: Tooltip {
            parent: content
            visible: true
            text: row.tooltipText
        }
    }

    readonly property bool renamingNothing: panel === null || panel.renamingKey === ""

    // A 10 px caps tag (design language: micro labels only): DEFAULT, CLAP, VST3.
    component MicroTag: Rectangle {
        id: tag
        property alias text: tagText.text
        anchors.verticalCenter: parent.verticalCenter
        width: tagText.implicitWidth + Theme.space(2)
        height: tagText.implicitHeight + Theme.space(1)
        radius: Theme.radiusBadge
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        Text {
            id: tagText
            anchors.centerIn: parent
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.capitalization: Font.AllUppercase
            font.pixelSize: Theme.textMicro
            font.letterSpacing: Theme.microTracking
            font.weight: Font.DemiBold
        }
    }
}
