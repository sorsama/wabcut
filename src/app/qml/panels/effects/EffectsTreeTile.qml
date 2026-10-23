pragma ComponentBehavior: Bound
import QtQuick

// One tile of the Effects panel's Icons view (UX rework; an item of an EffectsTree "tiles" row): a card with the large
// category icon, the name under it, a bookmark in the corner on favourites and DEFAULT on the default transitions. It
// behaves like a List-view row: click selects, double-click applies to every selected clip, drag onto the timeline,
// Effect Controls or a custom bin, right-click for the item menu.
Item {
    id: tile

    /// The item's roles (one map of the tiles row's `tiles`).
    property var item: ({})
    /// The EffectsPanel: its selection and actions.
    property var panel: null

    readonly property string rowKey: item.rowKey ?? ""
    readonly property string itemId: item.itemId ?? ""
    readonly property string name: item.name ?? ""
    readonly property bool plugin: item.plugin ?? false
    readonly property bool usable: (item.problem ?? "") === ""
    readonly property bool selected: panel !== null && panel.currentKey === rowKey
    readonly property bool isDefault: ((item.kind ?? "") === "videoTransition" || (item.kind ?? "") === "audioTransition")
                                      && panel !== null && panel.defaultTransitions.includes(itemId)
    readonly property string tip: (item.problem ?? "") !== "" ? item.problem : (item.description ?? "")
    /// The name is cut short (two lines in a narrow tile): the tooltip then shows it whole.
    readonly property bool nameElided: label.truncated
    /// The hover tooltip: the whole name, then the description (or the problem); "" = none.
    readonly property string tooltipText: tip !== "" ? name + " · " + tip : nameElided ? name : ""

    objectName: (item.home ?? false) ? (plugin ? "pluginItem_" : "effectItem_") + itemId
                                     : "effectItem_" + (item.parentKey ?? "") + "_" + itemId
    opacity: usable ? 1 : Theme.disabledOpacity + 0.2

    // The list keeps the keyboard; the selected tile is the accessible focus, so screen readers follow it as it moves.
    Accessible.role: Accessible.ListItem
    Accessible.name: plugin ? qsTr("%1, audio plug-in").arg(name) : name
    Accessible.description: tip
    Accessible.selectable: true
    Accessible.selected: selected
    Accessible.focusable: true
    Accessible.focused: selected && panel !== null && panel.treeFocused
    Accessible.onPressAction: tile.panel.activate(tile.rowKey)

    Rectangle {
        id: card
        anchors.fill: parent
        radius: Theme.radiusField
        color: tile.selected ? Theme.hover : hover.hovered ? Theme.raised : Theme.section
        border.width: tile.selected ? Theme.focusRingWidth : Theme.hairline
        border.color: tile.selected ? Theme.accentStroke : Theme.border

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: card.radius
            visible: tile.selected && tile.panel !== null && tile.panel.showFocus
        }
    }

    Icon {
        id: glyph
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.space(3.5)
        name: tile.item.icon ?? ""
        size: Theme.iconXl
        color: tile.selected || hover.hovered || drag.dragging ? Theme.accentStroke : Theme.textSecondary
    }
    Text {
        id: label
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: glyph.bottom
        anchors.topMargin: Theme.space(2)
        anchors.leftMargin: Theme.space(1.5)
        anchors.rightMargin: Theme.space(1.5)
        horizontalAlignment: Text.AlignHCenter
        text: tile.name
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        lineHeight: 0.95
        color: tile.usable ? Theme.textPrimary : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Theme.weightBody
    }

    // Favourite: the Favorites folder's bookmark, in the corner.
    Icon {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(1.5)
        visible: tile.item.favorite ?? false
        name: "bookmark"
        size: Theme.iconXs
        color: Theme.accentStroke
    }
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.space(1.5)
        visible: tile.isDefault || (tile.plugin && (tile.item.format ?? "") !== "")
        width: tagText.implicitWidth + Theme.space(2)
        height: tagText.implicitHeight + Theme.space(1)
        radius: Theme.radiusBadge
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        Text {
            id: tagText
            anchors.centerIn: parent
            text: tile.isDefault ? qsTr("Default") : (tile.item.format ?? "")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.capitalization: Font.AllUppercase
            font.pixelSize: Theme.textMicro
            font.letterSpacing: Theme.microTracking
            font.weight: Font.DemiBold
        }
    }

    HoverHandler {
        id: hover
        cursorShape: tile.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    // Click, double-click, right-click and the drag.
    EffectsDragSource {
        id: drag
        anchors.fill: parent
        itemId: tile.itemId
        imageItem: tile
        dragEnabled: tile.usable && tile.panel !== null && tile.panel.renamingKey === ""
        onTapped: tile.panel.pressRow(tile.rowKey)
        onDoubleTapped: tile.panel.activate(tile.rowKey)
        onMenuRequested: (x, y) => {
            tile.panel.pressRow(tile.rowKey)
            tile.panel.openMenu(tile.rowKey, tile, x, y)
        }
    }

    // A drop on a tile goes to its folder (a custom bin or a presets bin).
    DropArea {
        anchors.fill: parent
        onEntered: dragEvent => {
            dragEvent.accepted = tile.panel.dropEnter(tile.rowKey, dragEvent)
        }
        onExited: tile.panel.dropLeave(tile.rowKey)
        onDropped: dropEvent => tile.panel.dropOn(tile.rowKey, dropEvent)
    }

    Loader {
        active: hover.hovered && tile.tooltipText !== "" && !drag.dragging
        sourceComponent: Tooltip {
            parent: tile
            visible: true
            text: tile.tooltipText
        }
    }
}
