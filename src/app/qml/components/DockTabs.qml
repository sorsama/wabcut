pragma ComponentBehavior: Bound
import QtQuick

// A panel group's tabs (0.1.3 panel docking, design language v3): browser-style tabs over the shown panel's tab strip
// (DockSlot lays them there). The shown panel's tab rises from the strip in the panel's colour with flared feet; the
// others are a hue dot and a name, divided by hairlines. A click (or Left / Right) shows a panel; its × (or a middle
// click) closes it; dragging a tab moves just that panel: onto another group's tabs or middle to join it, near its edge
// to dock beside it, or outside the panels (or with Ctrl held) for a window of its own (DockManager). UX rework: a
// right-click (or the Menu key / Shift+F10) opens the tab's menu (DockTabMenu); a double-click maximizes the group.
// 0.4 (readable tabs): only the shown tab keeps room for its × (another tab shows its × under the pointer, over the end
// of its label). Short of room the tabs shrink in proportion to their spare room, never below their dot and about four
// characters (the shown tab: and its ×); when even that does not fit, the tabs that fit show in order - the shown one
// always among them - and the strip ends with a tab list button (`dockTabsOverflow`, "All panels in this group"): its
// menu (`dockTabsOverflowMenu`) lists every panel of the group in tab order with its icon, a check on the shown one and
// its focus key; choosing one shows it. Every tab's tooltip names its panel and its focus key ("Library  [Shift+1]").
// Its own QtQuick-only file: a file importing QtQuick.Controls would get Qt's ContextMenu instead of Wabcut's.
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

    /// The panels' focus keys (Premiere's Window shortcuts). Keep in sync with WindowMenu.qml's panelCommands.
    readonly property var panelCommands: ({ library: "window.projects", mediaBrowser: "window.mediaBrowser",
                                             effects: "window.effects", effectControls: "window.effectControls",
                                             source: "window.sourceMonitor", program: "window.programMonitor",
                                             timeline: "window.timeline", mixer: "window.audioTrackMixer" })

    /// The width every tab would take at its label's size.
    readonly property real naturalWidth: {
        let sum = 0
        for (let i = 0; i < root.naturalWidths.length; ++i)
            sum += root.naturalWidths[i]
        return sum
    }

    implicitHeight: Theme.tabStripHeight
    activeFocusOnTab: true

    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Panels in this group")

    // Arrow keys move between the tabs here: claim them so the keymap does not move the playhead. The Menu key and
    // Shift+F10 open the shown tab's menu.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Left || event.key === Qt.Key_Right
                                                       || root.isMenuKey(event)
    Keys.onLeftPressed: root.step(-1)
    Keys.onRightPressed: root.step(1)
    Keys.onPressed: event => {
        if (root.isMenuKey(event)) {
            event.accepted = true
            root.openMenuFor(root.current)
        }
    }
    function isMenuKey(event: KeyEvent): bool {
        return event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier) !== 0)
    }
    function step(delta: int) {
        const i = root.panels.indexOf(root.current)
        const next = root.panels[Math.max(0, Math.min(root.panels.length - 1, i + delta))]
        if (next && next !== root.current)
            root.activated(next)
    }
    /// The tab menu of `key`'s tab, under it.
    function openMenuFor(key: string) {
        const i = root.panels.indexOf(key)
        if (i < 0)
            return
        root.slot?.openTabMenu(key, root, root.tabX(i), root.height)
    }
    /// The focus key of `key`'s panel as the keymap shows it ("Shift+1"), or "".
    function shortcutOf(key: string): string {
        const command = root.panelCommands[key] ?? ""
        return command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    }

    FocusRing {
        targetRadius: Theme.radiusTab
        visible: root.activeFocus
    }

    // Each tab as wide as its dot, label and (the shown one) ×; narrower when the strip runs out of room (the labels then
    // elide), down to readable minimums; beyond that the tab list. Placed by hand, not by a Row: a Row places items it
    // gains later only in the window's next polish.
    FontMetrics {
        id: metrics
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Theme.weightLabel
    }
    /// The room for a tab's × (the shown tab keeps it; another tab shows it under the pointer).
    readonly property real closeRoom: Theme.controlHeightSmall + Theme.space(1)
    /// The tab list button and its gap.
    readonly property real overflowWidth: Theme.controlHeightSmall + Theme.space(1)
    /// A tab's dot, padding and, for the shown one, its ×.
    function chromeOf(key: string): real {
        return Theme.dotSize + Theme.space(5) + (key === root.current ? root.closeRoom : Theme.space(3))
    }
    readonly property var naturalWidths: root.panels.map(k => Math.ceil(metrics.advanceWidth(Workspaces.panelLabel(k)))
                                                              + root.chromeOf(k))
    /// The narrowest a tab gets: its dot and about four characters (or its whole label when shorter), and its chrome.
    function minTabWidth(index: int): real {
        const key = root.panels[index] ?? ""
        const label = Math.ceil(metrics.advanceWidth(Workspaces.panelLabel(key)))
        return Math.min(label, Math.ceil(metrics.advanceWidth("Abcd"))) + root.chromeOf(key)
    }
    /// Where the tabs go: { widths, shown (bool per tab), overflow (the tab list button shows) }. Arrays are copied
    /// (an array read back from a var property is not a JS array in typed code).
    function layoutTabs(): var {
        const n = root.panels.length
        const naturals = []
        const mins = []
        const shown = []
        for (let i = 0; i < n; ++i) {
            const natural = root.naturalWidths[i] ?? 0
            naturals.push(natural)
            mins.push(Math.min(natural, root.minTabWidth(i)))
            shown.push(true)
        }
        let natSum = 0
        let minSum = 0
        for (let i = 0; i < n; ++i) {
            natSum += naturals[i]
            minSum += mins[i]
        }
        let room = root.width
        if (natSum <= room)
            return { widths: naturals, shown: shown, overflow: false }
        let overflow = false
        if (minSum > room) {
            // Not even the narrowest tabs fit: the ones that do, in order (the shown one always), then the tab list.
            overflow = true
            room -= root.overflowWidth
            const cur = root.panels.indexOf(root.current)
            let used = cur >= 0 ? mins[cur] : 0
            let stopped = false
            for (let i = 0; i < n; ++i) {
                if (i === cur)
                    continue
                if (!stopped && used + mins[i] <= room) {
                    used += mins[i]
                } else {
                    stopped = true
                    shown[i] = false
                }
            }
            natSum = 0
            minSum = 0
            for (let i = 0; i < n; ++i) {
                if (shown[i]) {
                    natSum += naturals[i]
                    minSum += mins[i]
                }
            }
        }
        // Between the natural and the narrowest widths: each tab gives up the same share of its spare room.
        const k = natSum > minSum ? Math.max(0, Math.min(1, (room - minSum) / (natSum - minSum))) : 1
        const widths = []
        for (let i = 0; i < n; ++i)
            widths.push(shown[i] ? mins[i] + (naturals[i] - mins[i]) * k : 0)
        return { widths: widths, shown: shown, overflow: overflow }
    }
    readonly property var tabLayout: root.layoutTabs()
    readonly property var tabWidths: root.tabLayout.widths
    /// The tab list button shows (some tabs have no room).
    readonly property bool overflowing: root.tabLayout.overflow
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
                readonly property bool shown: root.tabLayout.shown[index] === true
                // The hairline after an inactive tab, unless the next one is the shown one (its flare meets it).
                readonly property bool divided: !active && index < root.panels.length - 1
                                                && root.panels[index + 1] !== root.current
                readonly property string shortcut: root.shortcutOf(tab.modelData)
                /// "Library  [Shift+1]": the tab's tooltip (its full name and its focus key).
                readonly property string tooltipText: tab.shortcut !== "" ? qsTr("%1  [%2]").arg(tab.label).arg(tab.shortcut)
                                                                          : tab.label

                objectName: "dockTab_" + modelData
                visible: tab.shown
                x: root.tabX(index)
                z: active ? 2 : 1
                height: row.height
                width: root.tabWidths[index] ?? 0
                opacity: dragging ? 0.6 : 1

                Accessible.role: Accessible.PageTab
                Accessible.name: label
                Accessible.description: tab.tooltipText
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
                    // The × keeps its room on the shown tab; on another tab the label elides under it while hovered.
                    trailingSpace: tab.active || hover.hovered ? closeButton.width + Theme.space(1) : Theme.space(3)
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
                // The full name and the focus key (the label may be elided).
                Loader {
                    active: hover.hovered && !tab.dragging && !closeButton.hovered
                    sourceComponent: Tooltip {
                        parent: tab
                        visible: true
                        text: tab.label
                        shortcut: tab.shortcut
                    }
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
                        // Premiere: a double-click on a panel's tab maximizes its frame (again: restores).
                        onDoubleTapped: root.slot?.toggleMaximize()
                    }
                    TapHandler {
                        acceptedButtons: Qt.MiddleButton
                        onTapped: root.closeRequested(tab.modelData)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: eventPoint => root.slot?.openTabMenu(tab.modelData, grabArea, eventPoint.position.x,
                                                                        eventPoint.position.y)
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

        // 0.4: the tab list (Premiere's panel group overflow »): every panel of the group, at the end of the shown tabs.
        IconButton {
            id: overflowButton
            objectName: "dockTabsOverflow"
            visible: root.overflowing
            x: root.tabX(root.panels.length) + Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: (Theme.tabStripHeight - Theme.tabHeight) / 2
            compact: true
            iconName: "chevron-down"
            tooltip: qsTr("All panels in this group")
            onClicked: overflowMenu.popup(overflowButton, 0, overflowButton.height)
        }
    }

    ContextMenu {
        id: overflowMenu
        objectName: "dockTabsOverflowMenu"

        Instantiator {
            model: root.panels
            delegate: ContextMenuItem {
                id: panelRow
                required property string modelData
                objectName: "dockTabsOverflow_" + panelRow.modelData
                text: Workspaces.panelLabel(panelRow.modelData)
                iconName: String(Workspaces.panel(panelRow.modelData)?.icon ?? "")
                command: root.panelCommands[panelRow.modelData] ?? ""
                checkable: true
                checked: panelRow.modelData === root.current
                onTriggered: {
                    panelRow.checked = Qt.binding(() => panelRow.modelData === root.current)
                    root.activated(panelRow.modelData)
                }
            }
            onObjectAdded: (index, object) => overflowMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => overflowMenu.removeItem(object as ContextMenuItem)
        }
    }
}
