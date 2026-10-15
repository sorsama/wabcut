pragma ComponentBehavior: Bound
import QtQuick

// A panel group's tabs (0.1.3 panel docking, design language v3): browser-style tabs over the shown panel's tab strip
// (DockSlot lays them there). The shown panel's tab rises from the strip in the panel's colour with flared feet; the
// others are a hue dot and a name, divided by hairlines. A click (or Left / Right) shows a panel; its × (or a middle
// click) closes it; dragging a tab moves just that panel: onto another group's tabs or middle to join it, near its edge
// to dock beside it, or outside the panels (or with Ctrl held) for a window of its own (DockManager).
Item {
    id: root

    /// The DockSlot these tabs belong to (typed Item: DockSlot itself creates them).
    required property Item host
    readonly property DockSlot slot: root.host as DockSlot
    /// The group's panels (Workspaces.catalog keys) and the one shown.
    property var panels: []
    property string current: ""
    signal activated(string key)
    signal closeRequested(string key)

    /// The width every tab would take at its label's size.
    readonly property real naturalWidth: root.naturalWidths.reduce((a, b) => a + b, 0)

    implicitHeight: Theme.tabStripHeight
    activeFocusOnTab: true

    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Panels in this group")

    // Arrow keys move between the tabs here: claim them so the keymap does not move the playhead.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Left || event.key === Qt.Key_Right
    Keys.onLeftPressed: root.step(-1)
    Keys.onRightPressed: root.step(1)
    function step(delta: int) {
        const i = root.panels.indexOf(root.current)
        const next = root.panels[Math.max(0, Math.min(root.panels.length - 1, i + delta))]
        if (next && next !== root.current)
            root.activated(next)
    }

    FocusRing {
        targetRadius: Theme.radiusTab
        visible: root.activeFocus
    }

    // Each tab as wide as its dot, label and ×, all narrower when the strip runs out of room (the labels then elide).
    // Placed by hand, not by a Row: a Row places items it gains later only in the window's next polish.
    FontMetrics {
        id: metrics
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Theme.weightLabel
    }
    readonly property real tabChrome: Theme.dotSize + Theme.space(5) + Theme.controlHeightSmall
    readonly property var naturalWidths: root.panels.map(k => Math.ceil(metrics.advanceWidth(Workspaces.panelLabel(k)))
                                                              + root.tabChrome)
    readonly property var tabWidths: {
        const total = root.naturalWidth
        if (total <= root.width || total <= 0)
            return root.naturalWidths
        const scale = root.width / total
        return root.naturalWidths.map(w => Math.max(Theme.space(14), w * scale))
    }
    function tabX(index: int): real {
        let x = 0
        for (let i = 0; i < index; ++i)
            x += root.tabWidths[i] ?? 0
        return x
    }

    Item {
        id: row
        anchors.fill: parent

        Repeater {
            model: root.panels

            delegate: Item {
                id: tab

                required property string modelData
                required property int index
                readonly property bool active: modelData === root.current
                readonly property bool dragging: drag.active
                readonly property string label: Workspaces.panelLabel(modelData)
                // The hairline after an inactive tab, unless the next one is the shown one (its flare meets it).
                readonly property bool divided: !active && index < root.panels.length - 1
                                                && root.panels[index + 1] !== root.current

                objectName: "dockTab_" + modelData
                x: root.tabX(index)
                z: active ? 2 : 1
                height: row.height
                width: root.tabWidths[index] ?? 0
                opacity: dragging ? 0.6 : 1

                Accessible.role: Accessible.PageTab
                Accessible.name: label
                Accessible.selected: active

                PanelTab {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Theme.tabHeight
                    text: tab.label
                    dotColor: Workspaces.panelHue(tab.modelData)
                    active: tab.active
                    hovered: hover.hovered
                    surfaceColor: Theme.panel
                    trailingSpace: closeButton.width + Theme.space(1)
                }

                Rectangle {
                    visible: tab.divided && !hover.hovered
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: (Theme.tabStripHeight - Theme.tabHeight) / 2
                    width: Theme.hairline
                    height: Theme.tabHeight - Theme.space(4)
                    color: Theme.borderStrong
                }

                IconButton {
                    id: closeButton
                    objectName: "dockTabClose_" + tab.modelData
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(1.5)
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: (Theme.tabStripHeight - Theme.tabHeight) / 2
                    compact: true
                    iconName: "x"
                    // Shown on the active tab and under the pointer, as browsers do.
                    opacity: tab.active || hover.hovered || activeFocus ? 1 : 0
                    tooltip: qsTr("Close %1").arg(tab.label)
                    onClicked: root.closeRequested(tab.modelData)
                }

                HoverHandler {
                    id: hover
                    cursorShape: tab.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                }
                // A click, a middle click or a drag on the tab (not on its ×: the button keeps its own presses).
                Item {
                    id: grabArea
                    anchors.left: parent.left
                    anchors.right: closeButton.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom

                    TapHandler {
                        onTapped: root.activated(tab.modelData)
                    }
                    TapHandler {
                        acceptedButtons: Qt.MiddleButton
                        onTapped: root.closeRequested(tab.modelData)
                    }
                    DragHandler {
                        id: drag
                        target: null
                        onActiveChanged: {
                            if (active) {
                                root.activated(tab.modelData)
                                root.slot?.beginPanelDrag(tab.modelData)
                            } else {
                                root.slot?.endDrag()
                            }
                        }
                        onCentroidChanged: {
                            if (active)
                                root.slot?.moveDrag(grabArea.mapToGlobal(centroid.position.x, centroid.position.y),
                                                   centroid.modifiers)
                        }
                    }
                }
            }
        }
    }
}
