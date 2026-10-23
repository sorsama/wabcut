pragma ComponentBehavior: Bound
import QtQuick

// A workbench panel (design language v3): an island on the black stage, radius 22, the panel surface, no shadow.
// Docked (the outermost panel of a DockSlot) it opens with a tab strip: a dark band with the panel's tab rising from it
// (hue dot and name; a group of several panels shows the group's DockTabs there instead, which DockSlot lays over the
// strip), then the `leading` tools, and at the right the `trailing` tools and the dock's grip, pop-out and close buttons
// (DockHandle). Undocked with a `title` it shows a "● Title" header. Either way a tool row hides when the strip has no
// room for it. Children go into the padded body below.
Rectangle {
    id: root

    property string title: ""
    /// The DockSlot this panel is the outermost panel of (found through its holder, which goes along when the slot
    /// floats), else null: a panel inside another one never carries the dock's handle. A binding over the whole parent
    /// chain, so it follows the panel when an ancestor moves (0.1.3 workspaces move panels between the columns).
    readonly property DockSlot dockHost: findDockHost()
    function findDockHost(): DockSlot {
        for (let p = root.parent; p; p = p.parent) {
            if (p instanceof Panel)
                return null
            if (p instanceof DockHolder)
                return (p as DockHolder).dockSlot as DockSlot
        }
        return null
    }
    property int padding: Theme.pad
    // Bottom padding follows the density (compact mode) unless the panel uses a custom padding.
    property int bottomPadding: padding === Theme.pad ? Theme.panelBottomPad : padding
    readonly property bool hasHeader: title !== ""
    /// Docked, in a group of several panels: the group's tabs (DockTabs, owned by the slot) cover the strip's left.
    readonly property bool tabbed: dockHost !== null && dockHost.tabbed
    /// The tab strip: docked panels with a title, and every panel of a tabbed group.
    readonly property bool hasStrip: dockHost !== null && (hasHeader || tabbed)
    readonly property bool hasTopRow: hasStrip || hasHeader
    default property alias contentData: body.data
    property alias leading: leadingRow.data
    property alias trailing: trailingRow.data
    readonly property alias body: body

    radius: Theme.radiusPanel
    color: Theme.panel
    border.width: Theme.hairline
    border.color: Theme.border
    // A screen reader names the region by its title (M5n).
    Accessible.role: Accessible.Pane
    Accessible.name: root.title

    // The tab strip's band: the panel's top corners, square at its foot (clipped from a taller rounded rectangle so its
    // corners match the panel's).
    Item {
        id: strip
        visible: root.hasStrip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.hasStrip ? Theme.tabStripHeight : 0
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: parent.height + root.radius * 2
            radius: root.radius
            color: Theme.sunken
        }

        // A lone panel's tab (a group's tabs are the slot's DockTabs, laid over this spot).
        PanelTab {
            id: ownTab
            objectName: "panelTab"
            visible: !root.tabbed
            x: Theme.space(3)
            anchors.bottom: parent.bottom
            width: Math.min(naturalWidth, Math.max(Theme.space(16), header.width * 0.5))
            text: root.dockHost ? Workspaces.panelLabel(root.dockHost.currentPanel) || root.title : root.title
            dotColor: root.dockHost ? Workspaces.panelHue(root.dockHost.currentPanel) : Theme.accentStroke
            surfaceColor: root.color
            trailingSpace: Theme.space(4)
        }
        // UX rework: a lone panel's tab works like a group's tab (DockTabs): dragging it moves the group, a right-click
        // opens the tab menu (DockTabMenu), a double-click maximizes the group. Docked panels only.
        Loader {
            active: root.dockHost !== null && !root.tabbed
            x: ownTab.x
            y: ownTab.y
            width: ownTab.width
            height: ownTab.height
            sourceComponent: Item {
                id: ownTabArea
                objectName: "panelTabArea"

                HoverHandler {
                    cursorShape: ownTabDrag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                }
                TapHandler {
                    onDoubleTapped: root.dockHost?.toggleMaximize()
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: eventPoint => root.dockHost?.openTabMenu(root.dockHost.currentPanel, ownTabArea,
                                                                       eventPoint.position.x, eventPoint.position.y)
                }
                DragHandler {
                    id: ownTabDrag
                    target: null
                    onActiveChanged: {
                        if (active)
                            root.dockHost?.beginPanelDrag(root.dockHost.currentPanel)
                        else
                            root.dockHost?.endDrag()
                    }
                    onCentroidChanged: {
                        if (active)
                            root.dockHost?.moveDrag(ownTabArea.mapToGlobal(centroid.position.x, centroid.position.y),
                                                    centroid.modifiers)
                    }
                }
            }
        }
    }

    Item {
        id: header
        visible: root.hasTopRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.hasStrip ? 0 : root.padding
        anchors.rightMargin: root.hasStrip ? Theme.space(2) : root.padding
        anchors.topMargin: root.hasStrip ? 0 : Theme.panelHeaderTop
        height: root.hasStrip ? Theme.tabStripHeight : root.hasHeader ? Theme.controlHeightSmall : 0

        // Where the tools start: after the tab (or the group's tabs) in the strip, after the title otherwise.
        readonly property real toolsX: root.hasStrip
                                       ? (root.tabbed ? Theme.space(3) + root.dockHost.tabsWidth : ownTab.x + ownTab.width)
                                         + Theme.space(4)
                                       : titleHeader.implicitWidth + Theme.space(3)
        // Room beside the tab or title (and the dock's handle): the trailing tools first, then the leading ones.
        readonly property real room: width - toolsX - (dockTools.width > 0 ? dockTools.width + Theme.space(2) : 0)

        SectionHeader {
            id: titleHeader
            visible: !root.hasStrip
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
        }

        Row {
            id: leadingRow
            visible: implicitWidth + (trailingRow.visible ? trailingRow.implicitWidth + Theme.space(2) : 0) <= header.room
            x: header.toolsX
            // In the strip the tools sit on the tab's line, a little below the band's middle.
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.hasStrip ? Theme.space(0.5) : 0
            spacing: Theme.space(1)
        }

        Row {
            id: trailingRow
            visible: implicitWidth <= header.room
            anchors.right: dockTools.left
            anchors.rightMargin: dockTools.width > 0 ? Theme.space(2) : 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.hasStrip ? Theme.space(0.5) : 0
            spacing: Theme.space(1)
        }

        Loader {
            id: dockTools
            active: root.dockHost !== null && root.hasStrip
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: Theme.space(0.5)
            sourceComponent: DockHandle {
                host: root.dockHost
            }
        }
    }

    // A dock's panel without a strip (a monitor switching with the other one): the handle over the top-right corner.
    HoverHandler {
        id: cornerHover
        enabled: root.dockHost !== null && !root.hasStrip
    }
    Loader {
        id: cornerTools
        active: root.dockHost !== null && !root.hasStrip
        z: 2
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space(2)
        anchors.rightMargin: Theme.space(2)
        opacity: cornerHover.hovered || ((cornerTools.item as DockHandle)?.busy ?? false) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        sourceComponent: DockHandle {
            host: root.dockHost
        }
    }

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.hasTopRow ? header.bottom : parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.padding
        anchors.rightMargin: root.padding
        anchors.topMargin: root.hasTopRow ? Theme.panelHeaderGap : root.padding
        anchors.bottomMargin: root.bottomPadding
    }
}
