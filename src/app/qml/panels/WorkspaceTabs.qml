pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The workspace switcher in the title bar (design language v3): a segmented pill group, the workspaces as pills on a
// panel-coloured track; the active one sits on an accent plate (black text) that slides between them (snap-out; no
// slide under reduce motion).
// 0.1.3 workspaces (Workspaces.qml): the tabs are the user's workspaces, in their order (none is built in). A tab's menu
// (right-click, the Menu key, Shift+F10) renames, duplicates, moves, resets or deletes it; a double-click (or F2)
// renames it in place; dragging a tab reorders the tabs. The "+" makes a new workspace from a preset (Edit, Color,
// Audio, Captions, Export) or as a copy of the current one. Tabs that no longer fit scroll (the wheel) and the chevron
// lists them all.
Item {
    id: root
    objectName: "workspaceTabs"

    property int current: 0
    signal activated(int workspace)

    // The tabs: the workspaces, in their order. `command`: the key that opens it (Premiere's workspace keys, Export
    // Media, for the first workspace of each preset), shown in its tooltip. Rebuilt only when what they show changes (a
    // workspace's layout changing keeps the tabs, and their focus).
    property var tabs: []
    function refreshTabs() {
        const next = Workspaces.all
            .map(w => ({ id: w.id, key: Workspaces.keyOf(w.id), label: w.name, hidden: false,
                         command: Workspaces.commandOf(w.id), hint: qsTr("%1 workspace").arg(w.name) }))
        if (JSON.stringify(next) !== JSON.stringify(root.tabs))
            root.tabs = next
    }
    Connections {
        target: Workspaces
        function onAllChanged() {
            root.refreshTabs()
        }
    }
    onCurrentChanged: refreshTabs()
    Component.onCompleted: refreshTabs()
    readonly property int currentIndex: tabs.findIndex(t => t.id === current)
    readonly property Item currentTab: repeater.count > 0 && currentIndex >= 0 ? repeater.itemAt(currentIndex) : null
    /// The tab of the first workspace made from the Export preset, if any (the onboarding tour points at it).
    readonly property Item exportTab: repeater.count > 0 ? repeater.itemAt(tabs.findIndex(t => t.key === "export")) : null
    /// The workspace whose tab is being renamed in place, or -1.
    property int renamingId: -1
    /// The workspace whose tab is being dragged (-1: none) and how far it has moved.
    property int draggingId: -1
    property real dragOffset: 0

    /// Keyboard focus to the active tab (M5j: where focus goes when the workspace shows no timeline).
    function focusCurrent() {
        if (root.currentTab)
            root.currentTab.forceActiveFocus(Qt.TabFocusReason)
    }
    function startRename(id: int) {
        root.renamingId = id
    }
    function focusTab(id: int) {
        for (let i = 0; i < repeater.count; ++i) {
            const tab = repeater.itemAt(i)
            if (tab && root.tabs[i].id === id)
                tab.forceActiveFocus(Qt.OtherFocusReason)
        }
    }
    // A new workspace: show it and name it.
    function adopt(id: int) {
        root.activated(id)
        root.startRename(id)
    }
    function openMenu(tab: Item, id: int, x: real, y: real) {
        tabMenu.workspaceId = id
        tabMenu.popup(tab, x, y)
    }
    // The strip scrolls so that `tab` is in view.
    function reveal(tab: Item) {
        if (!tab || strip.width <= 0)
            return
        if (tab.x < strip.contentX)
            strip.contentX = tab.x
        else if (tab.x + tab.width > strip.contentX + strip.width)
            strip.contentX = Math.min(strip.contentWidth - strip.width, tab.x + tab.width - strip.width)
    }
    onCurrentTabChanged: Qt.callLater(() => root.reveal(root.currentTab))

    // The track's inner margin around the pills.
    readonly property real inset: Theme.space(1)
    implicitWidth: row.implicitWidth + trailing.implicitWidth + Theme.space(1) + root.inset * 2
    implicitHeight: Theme.workspaceTabHeight + root.inset * 2
    // In the title bar's row: as wide as the tabs, narrower when the bar runs out of room (the strip then scrolls).
    Layout.fillWidth: true
    Layout.maximumWidth: implicitWidth
    Layout.preferredWidth: implicitWidth
    Layout.minimumWidth: Math.min(implicitWidth, Theme.space(64))

    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Workspaces")

    // The segmented group's track.
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.panel
    }

    Flickable {
        id: strip
        objectName: "workspaceStrip"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: root.inset
        width: Math.min(row.implicitWidth, root.width - trailing.width - Theme.space(1) - root.inset * 2)
        contentWidth: row.implicitWidth
        contentHeight: height
        clip: contentWidth > width
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        readonly property bool overflowing: contentWidth > width + 0.5

        Behavior on contentX {
            enabled: !Theme.reduceMotion
            NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
        }

        // The wheel scrolls tabs that do not fit.
        WheelHandler {
            enabled: strip.overflowing
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
                const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                strip.contentX = Math.max(0, Math.min(strip.contentWidth - strip.width, strip.contentX - delta / 2))
            }
        }

        // The active plate: an accent pill under the current tab.
        Rectangle {
            id: plate
            objectName: "workspacePlate"
            visible: root.currentTab !== null
            x: root.currentTab ? root.currentTab.x + (root.draggingId === root.current ? root.dragOffset : 0) : 0
            width: root.currentTab ? root.currentTab.width : 0
            height: strip.height
            color: Theme.accentFill
            radius: height / 2
            // It glides between tabs while the window is on screen. The tabs are built after the bar, so at start-up
            // (before the window shows) and in an offscreen grab it lands at once instead of sliding in from the left.
            readonly property bool glide: !Theme.reduceMotion && (plate.Window.window?.visible ?? false)

            Behavior on x {
                enabled: plate.glide && root.draggingId !== root.current
                NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
            }
            Behavior on width {
                enabled: plate.glide
                NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
            }
        }

        // Where a dragged tab would land: a slim accent pill between two tabs.
        Rectangle {
            id: dropMark
            objectName: "workspaceDropMark"
            property real at: 0
            visible: false
            x: at - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.focusRingWidth + 1
            height: strip.height - Theme.space(2)
            radius: width / 2
            color: Theme.accentStroke
        }

        Row {
            id: row
            height: strip.height

            Repeater {
                id: repeater
                model: root.tabs

                delegate: AbstractButton {
                    id: tab
                    required property var modelData
                    required property int index
                    readonly property int workspace: modelData.id
                    readonly property bool active: root.current === workspace
                    readonly property bool renaming: root.renamingId === workspace
                    readonly property bool dragging: drag.active
                    readonly property real dragOffset: drag.active ? drag.activeTranslation.x : 0

                    objectName: "workspaceTab_" + modelData.key
                    z: dragging ? 2 : 0
                    height: row.height
                    leftPadding: Theme.space(3.5)
                    rightPadding: Theme.space(3.5)
                    implicitWidth: (renaming ? Math.max(Theme.space(28), label.implicitWidth + Theme.space(6)) : label.implicitWidth)
                                   + leftPadding + rightPadding
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    transform: Translate { x: tab.dragOffset }
                    onClicked: root.activated(workspace)
                    onDoubleClicked: root.startRename(workspace)

                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.label
                    Accessible.selected: active

                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_F2) {
                            root.startRename(tab.workspace)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                            root.openMenu(tab, tab.workspace, 0, tab.height + Theme.space(1))
                            event.accepted = true
                        }
                    }

                    contentItem: Item {
                        implicitWidth: label.implicitWidth
                        implicitHeight: label.implicitHeight

                        Text {
                            id: label
                            anchors.fill: parent
                            visible: !tab.renaming
                            text: tab.modelData.label
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: tab.active ? Theme.accentOnFill : tab.hovered ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Theme.weightLabel

                            Behavior on color { ColorAnimation { duration: Theme.durFast } }
                        }
                        // Renaming in place: Enter (or leaving the field) keeps the name, Esc gives the old one back.
                        Loader {
                            anchors.fill: parent
                            anchors.leftMargin: -Theme.space(1.5)
                            anchors.rightMargin: -Theme.space(1.5)
                            active: tab.renaming
                            sourceComponent: FieldInput {
                                id: nameEditor
                                objectName: "workspaceRenameField"
                                property bool cancelled: false
                                property bool finished: false
                                implicitHeight: Theme.controlHeightSmall - Theme.space(1)
                                text: tab.modelData.label
                                maximumLength: 40
                                font.weight: Font.Medium
                                Accessible.name: qsTr("Workspace name")
                                Component.onCompleted: {
                                    selectAll()
                                    forceActiveFocus(Qt.OtherFocusReason)
                                }
                                // Later, not in this field's own handler: renaming rebuilds the tabs, and this field with them.
                                function finish(refocus: bool) {
                                    if (finished || root.renamingId !== tab.workspace)
                                        return
                                    finished = true
                                    const id = tab.workspace
                                    const name = text
                                    const keep = !cancelled
                                    Qt.callLater(() => {
                                        if (keep)
                                            Workspaces.rename(id, name)
                                        if (root.renamingId === id)
                                            root.renamingId = -1
                                        if (refocus)
                                            root.focusTab(id)
                                    })
                                }
                                onAccepted: finish(true)
                                onActiveFocusChanged: {
                                    if (!activeFocus)
                                        finish(false)
                                }
                                Keys.onEscapePressed: event => {
                                    cancelled = true
                                    finish(true)
                                    event.accepted = true
                                }
                            }
                        }
                    }
                    background: Rectangle {
                        radius: height / 2
                        color: !tab.active && (tab.hovered || tab.dragging) ? Theme.hover : "transparent"

                        FocusRing {
                            targetRadius: tab.height / 2
                            visible: tab.visualFocus
                        }
                    }

                    HoverHandler {
                        enabled: tab.enabled
                        cursorShape: tab.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: eventPoint => root.openMenu(tab, tab.workspace, eventPoint.position.x, eventPoint.position.y)
                    }
                    // Drag a tab along the bar to reorder.
                    DragHandler {
                        id: drag
                        enabled: !tab.renaming
                        target: null
                        xAxis.enabled: true
                        yAxis.enabled: false
                        // The tab's place among the others when it is let go: how many shown tabs end before its centre.
                        function landing(): int {
                            const centre = tab.x + tab.width / 2 + drag.activeTranslation.x
                            let place = 0
                            let edge = 0
                            for (let i = 0; i < repeater.count; ++i) {
                                const other = repeater.itemAt(i) as AbstractButton
                                if (!other || i === tab.index || root.tabs[i].hidden)
                                    continue
                                if (other.x + other.width / 2 < centre) {
                                    ++place
                                    edge = other.x + other.width
                                }
                            }
                            dropMark.at = place === 0 ? 0 : edge
                            return place
                        }
                        onActiveTranslationChanged: {
                            if (!active)
                                return
                            root.dragOffset = activeTranslation.x
                            landing()
                        }
                        onActiveChanged: {
                            dropMark.visible = active
                            root.draggingId = active ? tab.workspace : -1
                            root.dragOffset = 0
                            const place = landing()
                            if (active)
                                return
                            // Later: moving rebuilds the tabs, this one with them.
                            const id = tab.workspace
                            Qt.callLater(() => Workspaces.moveTo(id, place))
                        }
                    }

                    // The tooltip names the workspace's key (M5j: every tooltip shows its shortcut).
                    Loader {
                        active: tab.hovered && !tab.renaming && !tab.dragging
                        sourceComponent: Tooltip {
                            parent: tab
                            visible: true
                            text: tab.modelData.hint
                            shortcut: tab.modelData.command !== "" && KeymapDispatcher.revision >= 0
                                      ? KeymapDispatcher.shortcutText(tab.modelData.command) : ""
                        }
                    }
                }
            }
        }
    }

    Row {
        id: trailing
        anchors.right: parent.right
        anchors.rightMargin: root.inset
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0


        // All the workspaces, when their tabs no longer fit.
        IconButton {
            id: overflowButton
            objectName: "workspaceOverflowButton"
            visible: strip.overflowing
            compact: true
            iconName: "chevron-down"
            tooltip: qsTr("All workspaces")
            onClicked: listMenu.popup(overflowButton, 0, overflowButton.height + Theme.space(1))
        }
        IconButton {
            id: addButton
            objectName: "newWorkspaceButton"
            compact: true
            iconName: "plus"
            tooltip: qsTr("New workspace")
            onClicked: addMenu.popup(addButton, 0, addButton.height + Theme.space(1))
        }
    }

    WorkspaceTabMenu {
        id: tabMenu
        onRenameRequested: id => root.startRename(id)
        onCreated: id => root.adopt(id)
    }
    WorkspaceAddMenu {
        id: addMenu
        current: root.current
        onCreated: id => root.adopt(id)
    }
    WorkspaceListMenu {
        id: listMenu
        current: root.current
        onChosen: id => root.activated(id)
    }
}
