pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Settings window (0.1.3; it replaces the rail's Settings popover and the title bar's Appearance popover): a
// modal sheet over the window with the categories on the left (the slice mark on the current one) and the page on the
// right. Opened from the rail's Settings button, File > Settings… (Ctrl+,) and openAt(key) (e.g. "ai" from the
// transcript's "set up transcription"). Pages are created when first shown and kept while the window lives.
// Things outside the window (Settings Audio, Media & cache, the plug-in and extension managers, the shortcuts editor)
// open once it has closed, so they keep the focus (review P1-8).
Popup {
    id: root
    objectName: "settingsWindow"

    /// The page shown: one of `categories`' keys.
    property string currentPage: "general"
    /// The left rail's panel catalog (LeftRail.panels), for Settings > Interface's rail editor.
    property var railPanels: []
    /// Pages created so far ({key: true}).
    property var visited: ({ general: true })
    // What to run once the window has closed (runAction).
    property string pending: ""

    /// Settings Audio (the rail forwards it to the window's dialog).
    signal audioSettingsRequested()
    /// Help items the rail owns ("shortcuts", "trainer").
    signal helpRequested(string what)

    readonly property var categories: [
        { key: "general", label: qsTr("General"), icon: "settings" },
        { key: "appearance", label: qsTr("Appearance"), icon: "droplet" },
        { key: "interface", label: qsTr("Interface"), icon: "layout-grid" },
        { key: "timeline", label: qsTr("Timeline & editing"), icon: "film" },
        { key: "performance", label: qsTr("Playback & performance"), icon: "gauge" },
        { key: "audio", label: qsTr("Audio"), icon: "volume-2" },
        { key: "ai", label: qsTr("AI"), icon: "sparkles" },
        { key: "keyboard", label: qsTr("Keyboard"), icon: "keyboard" },
        { key: "extensions", label: qsTr("Plugins & extensions"), icon: "puzzle" }
    ]

    /// Shows the page `key` ("" or an unknown key keeps the current one) and opens the window.
    function openAt(key: string) {
        if (key !== "" && categories.some(c => c.key === key))
            showPage(key)
        open()
    }
    function showPage(key: string) {
        if (root.visited[key] !== true) {
            const next = Object.assign({}, root.visited)
            next[key] = true
            root.visited = next
        }
        root.currentPage = key
    }
    /// The page item for `key`, once created (tests, reveal()).
    function page(key: string): Item {
        for (let i = 0; i < pages.count; ++i) {
            const loader = pages.itemAt(i) as Loader
            if (loader && loader.objectName === "settingsPageLoader_" + key)
                return loader.item as Item
        }
        return null
    }
    /// Runs `what` once the window has closed (see the header): "media", "plugins", "audio", "extensions",
    /// "shortcuts", "trainer" or "editing" (0.2: Preferences > Media and Timeline).
    function runAction(what: string) {
        if (!root.visible) {
            root.dispatch(what)
            return
        }
        root.pending = what
        root.close()
    }
    function dispatch(what: string) {
        if (what === "media")
            MediaReadiness.openSettings()
        else if (what === "plugins")
            Plugins.openSettings()
        else if (what === "audio")
            root.audioSettingsRequested()
        else if (what === "extensions")
            Extensions.openManager()
        else if (what === "shortcuts" || what === "trainer")
            root.helpRequested(what)
        else if (what === "editing")
            Editing.runCommand("edit.preferencesMedia")
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: Math.min(Theme.space(264), (parent ? parent.width : Theme.windowDefaultWidth) - Theme.space(16))
    height: Math.min(Theme.space(190), (parent ? parent.height : Theme.windowDefaultHeight) - Theme.space(16))
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape

    onOpened: Qt.callLater(() => categoryList.focusCurrent())
    onClosed: {
        const what = root.pending
        root.pending = ""
        if (what !== "")
            root.dispatch(what)
    }

    Overlay.modal: Rectangle {
        objectName: "settingsScrim"
        color: Theme.scrim
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }

    contentItem: Item {
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Settings")

        // ---- Categories ----
        Rectangle {
            id: sidebar
            width: Theme.space(58)
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            radius: Theme.radiusPanel
            color: Theme.sunken

            // Square the sidebar's right edge against the page.
            Rectangle {
                anchors.right: parent.right
                width: Theme.radiusPanel
                height: parent.height
                color: parent.color
            }

            Text {
                id: windowTitle
                x: Theme.space(5)
                y: Theme.space(5)
                text: qsTr("Settings")
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textTitle
                font.weight: Theme.weightTitle
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            Column {
                id: categoryList
                anchors.top: windowTitle.bottom
                anchors.topMargin: Theme.space(4)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(0.5)
                Accessible.role: Accessible.PageTabList
                Accessible.name: qsTr("Settings categories")

                // On open the current category takes the focus without its ring (the pointer opened it); Tab and the
                // arrows show the ring from there.
                function focusCurrent() {
                    const index = root.categories.findIndex(c => c.key === root.currentPage)
                    const item = categoryRepeater.itemAt(Math.max(0, index))
                    if (item)
                        item.forceActiveFocus(Qt.PopupFocusReason)
                }
                function stepFrom(from: int, step: int) {
                    const count = root.categories.length
                    const index = (from + step + count) % count
                    root.showPage(root.categories[index].key)
                    categoryRepeater.itemAt(index)?.forceActiveFocus(Qt.TabFocusReason)
                }

                Repeater {
                    id: categoryRepeater
                    model: root.categories

                    delegate: AbstractButton {
                        id: categoryButton
                        required property var modelData
                        required property int index
                        readonly property bool current: root.currentPage === modelData.key

                        objectName: "settingsCategory_" + modelData.key
                        width: categoryList.width
                        height: Theme.controlHeight
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        checkable: false
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.label
                        Accessible.checked: current
                        onClicked: root.showPage(modelData.key)
                        // Up/Down walk the categories (and show them), like a tab list.
                        Keys.onUpPressed: categoryList.stepFrom(index, -1)
                        Keys.onDownPressed: categoryList.stepFrom(index, 1)
                        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Up || event.key === Qt.Key_Down

                        // A pill row, like Home's navigation (design language v3).
                        background: Rectangle {
                            radius: height / 2
                            color: categoryButton.current ? Theme.hover
                                 : categoryButton.hovered ? Qt.alpha(Theme.hover, 0.6) : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durFast } }

                            FocusRing {
                                targetRadius: categoryButton.height / 2
                                visible: categoryButton.visualFocus
                            }
                        }
                        contentItem: Item {
                            SliceMark {
                                visible: categoryButton.current
                                anchors.left: parent.left
                                anchors.leftMargin: Theme.space(1.5)
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.accentStroke
                            }
                            Icon {
                                id: categoryIcon
                                x: Theme.space(5)
                                anchors.verticalCenter: parent.verticalCenter
                                name: categoryButton.modelData.icon
                                size: Theme.iconSm
                                color: categoryButton.current ? Theme.textPrimary : Theme.textSecondary
                            }
                            Text {
                                anchors.left: categoryIcon.right
                                anchors.leftMargin: Theme.space(2)
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: categoryButton.modelData.label
                                elide: Text.ElideRight
                                color: categoryButton.current ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: categoryButton.current ? Theme.weightLabel : Font.Medium

                            }
                        }
                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }

        // ---- The page ----
        Item {
            id: pageArea
            anchors.left: sidebar.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            Repeater {
                id: pages
                model: root.categories

                delegate: Loader {
                    required property var modelData
                    readonly property string pageKey: modelData.key
                    objectName: "settingsPageLoader_" + modelData.key
                    anchors.fill: parent
                    active: root.visited[pageKey] === true
                    visible: root.currentPage === pageKey
                    sourceComponent: pageKey === "general" ? generalPage
                                   : pageKey === "appearance" ? appearancePage
                                   : pageKey === "interface" ? interfacePage
                                   : pageKey === "timeline" ? timelinePage
                                   : pageKey === "performance" ? performancePage
                                   : pageKey === "audio" ? audioPage
                                   : pageKey === "ai" ? aiPage
                                   : pageKey === "keyboard" ? keyboardPage
                                   : extensionsPage
                }
            }

            IconButton {
                objectName: "settingsClose"
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Theme.space(3)
                iconName: "x"
                tooltip: qsTr("Close")
                onClicked: root.close()
            }
        }
    }

    Component {
        id: generalPage
        GeneralSettingsPage {
            onActionRequested: what => root.runAction(what)
        }
    }
    Component {
        id: appearancePage
        AppearanceSettingsPage {}
    }
    Component {
        id: interfacePage
        InterfaceSettingsPage {
            railPanels: root.railPanels
        }
    }
    Component {
        id: timelinePage
        TimelineSettingsPage {
            onActionRequested: what => root.runAction(what)
        }
    }
    Component {
        id: performancePage
        PerformanceSettingsPage {
            onActionRequested: what => root.runAction(what)
        }
    }
    Component {
        id: audioPage
        SettingsPage {
            objectName: "audioSettingsPage"
            title: qsTr("Audio")
            description: qsTr("Recording, monitoring and the audio plug-ins Wabcut loads.")

            SettingsSection {
                title: qsTr("Voice-over")
                SettingsRow {
                    label: qsTr("Microphone, count-in and latency")
                    description: qsTr("The input voice-over records from, its pre-roll and post-roll, monitoring, and the recording latency.")
                    PillButton {
                        objectName: "audioSettingsButton"
                        iconName: "mic"
                        text: qsTr("Open…")
                        onClicked: root.runAction("audio")
                    }
                }
            }
            SettingsSection {
                title: qsTr("Plug-ins")
                SettingsRow {
                    label: qsTr("Audio plug-ins")
                    description: qsTr("Plug-in folders, rescanning, and plug-ins that are blocked after a crash.")
                    PillButton {
                        objectName: "audioPluginsSettingsButton"
                        iconName: "sliders-horizontal"
                        text: qsTr("Open…")
                        onClicked: root.runAction("plugins")
                    }
                }
            }
        }
    }
    Component {
        id: aiPage
        AiSettingsPage {
            settingsOpen: root.opened
        }
    }
    Component {
        id: keyboardPage
        SettingsPage {
            objectName: "keyboardSettingsPage"
            title: qsTr("Keyboard")
            description: qsTr("Every command's keys, Premiere-compatible key sets, and a trainer to learn them.")

            SettingsSection {
                title: qsTr("Shortcuts")
                SettingsRow {
                    label: qsTr("Keyboard shortcuts")
                    description: qsTr("Change keys, import a Premiere key set, or reset to the defaults.")
                    PillButton {
                        objectName: "keyboardShortcutsButton"
                        iconName: "keyboard"
                        text: qsTr("Open…")
                        command: "edit.keyboardShortcuts"
                        onClicked: root.runAction("shortcuts")
                    }
                }
                SettingsRow {
                    label: qsTr("Keyboard trainer")
                    description: qsTr("A short game that teaches the editing keys.")
                    PillButton {
                        objectName: "keyboardTrainerButton"
                        iconName: "zap"
                        text: qsTr("Start…")
                        onClicked: root.runAction("trainer")
                    }
                }
            }
        }
    }
    Component {
        id: extensionsPage
        SettingsPage {
            objectName: "extensionsSettingsPage"
            title: qsTr("Plugins & extensions")
            description: qsTr("Extensions add commands and panels; audio plug-ins add effects.")

            SettingsSection {
                title: qsTr("Extensions")
                SettingsRow {
                    label: qsTr("Installed and project extensions")
                    description: qsTr("Turn extensions on or off, review their permissions and read their logs.")
                    PillButton {
                        objectName: "extensionsSettingsButton"
                        iconName: "puzzle"
                        text: qsTr("Manage…")
                        onClicked: root.runAction("extensions")
                    }
                }
            }
            SettingsSection {
                title: qsTr("Audio plug-ins")
                SettingsRow {
                    label: qsTr("Plug-in folders and scanning")
                    description: qsTr("Where Wabcut looks for VST3 and CLAP plug-ins, and the ones it blocks.")
                    PillButton {
                        iconName: "sliders-horizontal"
                        text: qsTr("Open…")
                        onClicked: root.runAction("plugins")
                    }
                }
            }
        }
    }
}
