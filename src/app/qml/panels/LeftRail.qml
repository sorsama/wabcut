pragma ComponentBehavior: Bound
import QtQuick

// The panel switcher (design language v2): a narrow rail whose icons open/focus the left browser panel (Library,
// Effects, Transcript (key "captions"), Audio Tools, Markers, ...); the active one carries the slice mark. Bottom: Help
// (M5j: the welcome, the tour, the Keyboard Trainer, What's new, the shortcuts) and Settings (0.1.3: the Settings
// window, hosted here; it replaced the Settings popover and the title bar's Appearance popover). Workspaces are the
// title bar's tabs. Preferences persist through Preferences.qml.
//
// 0.1.3, customizable: the entries follow the user's order and can be hidden (Preferences.railLayout, edited in
// Settings > Interface or from the rail's right-click menu); `panels` stays the whole catalog for Main (labels, keys).
// The entries scroll when the window is too short for all of them. With AI turned off the Generate entry is hidden.
Panel {
    id: root

    objectName: "leftRail"

    property string panel: "library"        // active browser panel key
    signal panelRequested(string key)
    /// Help (M5j onboarding): "welcome", "tour", "trainer", "whatsNew" or "shortcuts".
    signal helpRequested(string what)
    /// Settings > Audio (M5m, A.8: microphone, voice-over options, latency); the window owns the dialog.
    signal audioSettingsRequested()

    /// Opens the Settings window on page `key` ("" keeps the page it showed last; see SettingsWindow.categories).
    function openSettings(key: string) {
        settingsLoader.active = true
        // Its own statement: a line that starts with "(" would continue the one above ("true(...)").
        const settings = settingsLoader.item as SettingsWindow
        settings?.openAt(key)
    }
    /// Settings > AI (the transcript panel's "set up transcription" opens it too).
    function openAiPreferences() {
        root.openSettings("ai")
    }
    /// Settings > Interface, scrolled to the rail's editor.
    function customizeRail() {
        root.openSettings("interface")
        const settings = settingsLoader.item as SettingsWindow
        Qt.callLater(() => (settings?.page("interface") as InterfaceSettingsPage)?.revealRail())
    }

    padding: Theme.space(2)

    readonly property var panels: [
        { key: "library", label: qsTr("Library"), icon: "library", command: "window.projects" },
        { key: "mediaBrowser", label: qsTr("Media Browser"), icon: "folder-open", command: "window.mediaBrowser" },
        { key: "effects", label: qsTr("Effects"), icon: "sparkles", command: "window.effects" },
        { key: "captions", label: qsTr("Transcript"), icon: "captions" },
        { key: "audio", label: qsTr("Audio Tools"), icon: "audio-lines" },
        { key: "essential", label: qsTr("Essential Sound"), icon: "sliders-horizontal" },
        { key: "graphics", label: qsTr("Callouts"), icon: "layers" },
        { key: "highlights", label: qsTr("Highlights"), icon: "zap" },
        { key: "markers", label: qsTr("Markers"), icon: "bookmark" },
        { key: "metadata", label: qsTr("Metadata"), icon: "tag" },
        { key: "production", label: qsTr("Production"), icon: "folders" },
        { key: "review", label: qsTr("Review"), icon: "message-square" },
        { key: "shorts", label: qsTr("Shorts"), icon: "clapperboard" },
        { key: "takes", label: qsTr("Takes"), icon: "repeat" },
        { key: "generate", label: qsTr("Generate"), icon: "sparkles" }
    ]
    /// Every entry in the user's order ([{key, label, icon, command, hidden}]).
    readonly property var entries: Preferences.railEntries(root.panels)
    /// The entries the rail shows.
    readonly property var visiblePanels: root.entries.filter(e => !e.hidden && (e.key !== "generate" || Preferences.aiEnabled))

    // The rail's right-click menu works on this entry ("" = the rail itself).
    property string menuKey: ""
    function moveEntry(key: string, step: int) {
        const order = root.entries.map(e => e.key)
        const from = order.indexOf(key)
        const to = from + step
        if (from < 0 || to < 0 || to >= order.length)
            return
        order.splice(to, 0, order.splice(from, 1)[0])
        Preferences.setRailLayout(order, root.entries.filter(e => e.hidden).map(e => e.key))
    }
    function hideEntry(key: string) {
        const hidden = root.entries.filter(e => e.hidden).map(e => e.key)
        if (!hidden.includes(key))
            hidden.push(key)
        Preferences.setRailLayout(root.entries.map(e => e.key), hidden)
    }

    // 0.1.3: with AI turned off, its offline mode stays on (also if something reset it since).
    Component.onCompleted: {
        if (!Preferences.aiEnabled && !AiController.offlineMode)
            AiController.offlineMode = true
    }
    // Settings > General's autosave interval (0: ProjectFile's own).
    Binding {
        target: ProjectFile
        property: "autosaveIntervalSeconds"
        value: Preferences.autosaveSeconds
        when: Preferences.autosaveSeconds > 0
        restoreMode: Binding.RestoreNone
    }

    // A right-click on the rail: its menu, about the entry under the pointer if there is one.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint, button) => {
            const p = root.mapToItem(entryColumn, eventPoint.position.x, eventPoint.position.y)
            const entry = entryColumn.childAt(p.x, p.y)
            const name = entry ? entry.objectName : ""
            root.menuKey = name.startsWith("railButton_") ? name.slice("railButton_".length) : ""
            railMenu.popup(root, eventPoint.position.x, eventPoint.position.y)
        }
    }

    Flickable {
        id: entryScroll
        anchors.top: parent.top
        anchors.bottom: footer.top
        anchors.bottomMargin: Theme.space(2)
        anchors.horizontalCenter: parent.horizontalCenter
        width: entryColumn.width + Theme.space(4)
        contentWidth: width
        contentHeight: entryColumn.height
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        clip: contentHeight > height

        Column {
            id: entryColumn
            objectName: "railEntries"
            x: Theme.space(2)
            spacing: Theme.space(2)

            Repeater {
                model: root.visiblePanels

                delegate: IconButton {
                    id: railButton
                    required property var modelData

                    objectName: "railButton_" + modelData.key
                    iconName: modelData.icon
                    tooltip: modelData.label
                    command: modelData.command ?? ""
                    checkable: true
                    checked: root.panel === modelData.key
                    // Radio-like: clicking the active panel keeps it active (and focuses it again).
                    checkedColor: Theme.hover
                    checkedIconColor: Theme.textPrimary
                    onClicked: {
                        checked = Qt.binding(() => root.panel === modelData.key)
                        root.panelRequested(modelData.key)
                    }
                    // Keyboard focus scrolls a hidden entry into view.
                    onActiveFocusChanged: {
                        if (activeFocus)
                            entryScroll.contentY = Math.max(0, Math.min(y, Math.max(0, entryScroll.contentHeight - entryScroll.height),
                                                                        Math.max(entryScroll.contentY, y + height - entryScroll.height)))
                    }

                    // The active entry's slice mark, at the rail's edge.
                    SliceMark {
                        visible: railButton.checked
                        anchors.right: parent.left
                        anchors.rightMargin: Theme.space(1) - Theme.hairline
                        anchors.verticalCenter: parent.verticalCenter
                        height: Theme.space(4)
                        color: Theme.accentStroke
                    }
                }
            }
        }
    }

    ContextMenu {
        id: railMenu
        objectName: "railMenu"
        readonly property var entry: root.entries.find(e => e.key === root.menuKey) ?? null
        readonly property int entryIndex: root.entries.findIndex(e => e.key === root.menuKey)

        ContextMenuItem {
            objectName: "railMenuHide"
            visible: railMenu.entry !== null
            height: visible ? implicitHeight : 0
            text: qsTr("Hide %1").arg(railMenu.entry?.label ?? "")
            iconName: "eye-off"
            onTriggered: root.hideEntry(root.menuKey)
        }
        ContextMenuItem {
            visible: railMenu.entry !== null
            height: visible ? implicitHeight : 0
            enabled: railMenu.entryIndex > 0
            text: qsTr("Move up")
            iconName: "chevron-up"
            onTriggered: root.moveEntry(root.menuKey, -1)
        }
        ContextMenuItem {
            visible: railMenu.entry !== null
            height: visible ? implicitHeight : 0
            enabled: railMenu.entryIndex >= 0 && railMenu.entryIndex < root.entries.length - 1
            text: qsTr("Move down")
            iconName: "chevron-down"
            onTriggered: root.moveEntry(root.menuKey, 1)
        }
        ContextMenuSeparator {
            visible: railMenu.entry !== null
            height: visible ? implicitHeight : 0
        }
        ContextMenuItem {
            objectName: "railMenuCustomize"
            text: qsTr("Customize rail…")
            iconName: "layout-grid"
            onTriggered: root.customizeRail()
        }
        ContextMenuItem {
            objectName: "railMenuReset"
            text: qsTr("Reset rail")
            iconName: "rotate-ccw"
            enabled: Preferences.railLayout !== ""
            onTriggered: Preferences.setRailLayout([], [])
        }
    }

    Column {
        id: footer
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.compact ? Theme.space(1) : Theme.space(2)

        // Help (M5j): the welcome, the tour, the Keyboard Trainer, What's new and the shortcuts.
        IconButton {
            id: helpButton
            objectName: "helpButton"
            anchors.horizontalCenter: parent.horizontalCenter
            iconName: "circle-help"
            tooltip: qsTr("Help")
            checkable: true
            checked: helpPopover.opened
            onClicked: {
                checked = Qt.binding(() => helpPopover.opened)
                if (helpPopover.opened)
                    helpPopover.close()
                else
                    helpPopover.open()
            }

            Popover {
                id: helpPopover
                objectName: "helpPopover"
                // What to run once the popover has closed and given focus back, so the dialog keeps the focus.
                property string pending: ""
                title: qsTr("Help")
                x: helpButton.width + Theme.space(4)
                y: helpButton.height - height
                transformOrigin: Item.BottomLeft
                onClosed: {
                    const what = helpPopover.pending
                    helpPopover.pending = ""
                    if (what !== "")
                        root.helpRequested(what)
                }

                Column {
                    spacing: Theme.space(1)

                    Repeater {
                        model: [
                            { key: "welcome", icon: "house", label: qsTr("Welcome and setup…") },
                            { key: "tour", icon: "arrow-right", label: qsTr("Take the tour") },
                            { key: "trainer", icon: "zap", label: qsTr("Keyboard Trainer…") },
                            { key: "whatsNew", icon: "sparkles", label: qsTr("What's new") },
                            { key: "shortcuts", icon: "keyboard", label: qsTr("Keyboard shortcuts…"), command: "edit.keyboardShortcuts" }
                        ]
                        delegate: PillButton {
                            required property var modelData
                            objectName: "helpItem_" + modelData.key
                            width: Theme.space(56)
                            variant: "ghost"
                            iconName: modelData.icon
                            text: modelData.label
                            command: modelData.command ?? ""
                            onClicked: {
                                helpPopover.pending = modelData.key
                                helpPopover.close()
                            }
                        }
                    }
                }
            }
        }

        // Settings (0.1.3): the Settings window.
        IconButton {
            id: settingsButton
            objectName: "settingsButton"
            anchors.horizontalCenter: parent.horizontalCenter
            iconName: "settings"
            tooltip: qsTr("Settings")
            shortcut: Qt.platform.os === "osx" ? "⌘," : "Ctrl+,"
            onClicked: root.openSettings("")
        }
    }

    // The Settings window: created on first use, then kept (its pages remember where they were scrolled).
    Loader {
        id: settingsLoader
        objectName: "settingsWindowLoader"
        active: false
        sourceComponent: SettingsWindow {
            railPanels: root.panels
            onAudioSettingsRequested: root.audioSettingsRequested()
            onHelpRequested: what => root.helpRequested(what)
        }
    }
}
