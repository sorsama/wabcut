pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects

// The Settings window (0.1.3; it replaced the rail's Settings popover and the title bar's Appearance popover): a
// modal sheet over the window with the categories on the left (the slice mark on the current one) and the page on the
// right. Opened from Edit > Settings… (Ctrl+,), Home's Settings button and openAt(key) (e.g. "ai" from the
// transcript's "set up transcription"); Main hosts it (window.openSettings). Pages are created when first shown and kept while the window lives.
// Things outside the window (Settings Audio, Media & cache, the plug-in and extension managers, the shortcuts editor)
// open once it has closed, so they keep the focus (review P1-8).
// UX rework (stream L): a search field tops the sidebar (it has the focus when the window opens). While it has text,
// the sidebar lists the settings it finds (AppSettings.search: title, "Page › Section", a mark on rows that open a
// dialog of their own) instead of the categories; choosing one shows its page, scrolls to the row, flashes it and
// focuses its control. revealSetting(page, row) does the same from outside (AppSettings.openRequested: the command
// palette's Settings rows).
Popup {
    id: root
    objectName: "settingsWindow"

    /// The page shown: one of `categories`' keys.
    property string currentPage: "general"
    /// Pages created so far ({key: true}).
    property var visited: ({ general: true })
    // What to run once the window has closed (runAction).
    property string pending: ""
    /// The Settings search's text ("" = the categories show).
    property alias searchText: searchField.text
    /// What the search finds (AppSettings.search: { id, page, row, title, detail, opens, ... }), best first.
    readonly property var results: searchText.trim() !== "" ? AppSettings.search(searchText, 12) : []
    // A row to show once the window has opened ({ page, row }; revealSetting before the window was open).
    property var pendingReveal: null

    /// Settings Audio (Main opens the window's dialog).
    signal audioSettingsRequested()
    /// Help items the window owns ("shortcuts", "trainer": Main.showHelp).
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
    /// Opens the window on page `pageKey` and shows its setting `row` (an AppSettings.index `row`): the row scrolls into
    /// view, flashes, and its control takes the focus. The search is cleared, so the categories show. Used for
    /// AppSettings.openRequested (the command palette's Settings rows); "" = the page only, like openAt.
    function revealSetting(pageKey: string, row: string) {
        root.searchText = ""
        root.showRow(pageKey, row)
        root.open()
    }
    /// Shows page `pageKey` and then its row `row`, now if the window is open, else once it has opened.
    function showRow(pageKey: string, row: string) {
        if (!root.categories.some(c => c.key === pageKey))
            return
        root.showPage(pageKey)
        if (row === "")
            return
        if (root.opened)
            Qt.callLater(() => root.revealRow(pageKey, row))
        else
            root.pendingReveal = { page: pageKey, row: row }
    }
    /// The row `row` of the page `pageKey` (created and shown): scroll, flash, focus. False when it has no such row.
    function revealRow(pageKey: string, row: string): bool {
        const item = root.page(pageKey)
        if (pageKey === "ai")
            return (item as AiSettingsPage)?.reveal(row) ?? false
        return (item as SettingsPage)?.reveal(row) ?? false
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

    // Opened from outside on a row: that row (with its control focused); else the search field, ready to type.
    onOpened: {
        const reveal = root.pendingReveal
        root.pendingReveal = null
        if (reveal)
            Qt.callLater(() => root.revealRow(reveal.page, reveal.row))
        else
            Qt.callLater(() => searchField.forceActiveFocus(Qt.PopupFocusReason))
    }
    onClosed: {
        root.searchText = "" // the categories show again next time
        root.pendingReveal = null
        const what = root.pending
        root.pending = ""
        if (what !== "")
            root.dispatch(what)
    }

    Overlay.modal: Rectangle {
        objectName: "settingsScrim"
        color: Theme.scrim
    }

    // A raised sheet (design language: dialogs and menus cast a soft shadow): the hairline ring and a soft shadow under
    // it, so the window reads as lifted off what is behind it (in the light theme its white edge met the light editor).
    background: Rectangle {
        objectName: "settingsWindowBackground"
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong

        RectangularShadow {
            objectName: "settingsWindowShadow"
            z: -1
            anchors.fill: parent
            radius: parent.radius
            offset.y: Theme.space(3)
            blur: Theme.space(14)
            color: Theme.shadow
        }
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

            // ---- Search ----
            FieldInput {
                id: searchField
                objectName: "settingsSearch"
                anchors.top: windowTitle.bottom
                anchors.topMargin: Theme.space(3)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Search settings")
                Accessible.name: qsTr("Search settings")
                Accessible.description: root.searchText.trim() === "" ? ""
                                        : qsTr("%n setting(s) found", "", root.results.length)
                // Enter shows the best match; Down goes into the results (or the categories).
                Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Down
                onAccepted: {
                    if (root.results.length > 0)
                        resultList.choose(0)
                }
                Keys.onDownPressed: {
                    if (root.searchText.trim() !== "")
                        resultList.focusResult(0)
                    else
                        categoryList.focusCurrent()
                }
            }

            // ---- Search results (while the field has text) ----
            ListView {
                id: resultList
                objectName: "settingsResults"
                anchors.top: searchField.bottom
                anchors.topMargin: Theme.space(3)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                anchors.bottomMargin: Theme.space(3)
                visible: root.searchText.trim() !== ""
                clip: true
                spacing: Theme.space(0.5)
                boundsBehavior: Flickable.StopAtBounds
                model: root.results
                ScrollBar.vertical: ThinScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Settings found")

                function focusResult(index: int) {
                    if (index < 0) {
                        searchField.forceActiveFocus(Qt.TabFocusReason)
                        return
                    }
                    if (index >= count)
                        return
                    positionViewAtIndex(index, ListView.Contain)
                    itemAtIndex(index)?.forceActiveFocus(Qt.TabFocusReason)
                }
                function choose(index: int) {
                    const hit = root.results[index]
                    if (!hit)
                        return
                    root.showPage(hit.page)
                    Qt.callLater(() => root.revealRow(hit.page, hit.row))
                }

                delegate: AbstractButton {
                    id: resultButton
                    required property var modelData
                    required property int index
                    readonly property var category: root.categories.find(c => c.key === modelData.page) ?? null
                    readonly property bool opensDialog: (modelData.opens ?? "") !== ""

                    objectName: "settingsResult_" + modelData.id
                    width: resultList.width
                    height: resultTexts.implicitHeight + Theme.space(4)
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.title
                    Accessible.description: opensDialog ? qsTr("%1, opens its own window").arg(modelData.detail)
                                                        : modelData.detail
                    onClicked: resultList.choose(index)
                    Keys.onUpPressed: resultList.focusResult(index - 1)
                    Keys.onDownPressed: resultList.focusResult(index + 1)
                    Keys.onReturnPressed: resultList.choose(index)
                    Keys.onEnterPressed: resultList.choose(index)
                    Keys.onShortcutOverride: event => event.accepted = [Qt.Key_Up, Qt.Key_Down, Qt.Key_Return, Qt.Key_Enter].includes(event.key)

                    background: Rectangle {
                        radius: Theme.radiusField
                        color: resultButton.pressed ? Theme.hover
                             : resultButton.hovered || resultButton.activeFocus ? Qt.alpha(Theme.hover, 0.6) : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.durFast } }

                        FocusRing {
                            targetRadius: Theme.radiusField
                            visible: resultButton.visualFocus
                        }
                    }
                    contentItem: Item {
                        Icon {
                            id: resultIcon
                            x: Theme.space(2.5)
                            y: Theme.space(2)
                            name: resultButton.category?.icon ?? "settings"
                            size: Theme.iconSm
                            color: Theme.textSecondary
                        }
                        Column {
                            id: resultTexts
                            anchors.left: resultIcon.right
                            anchors.leftMargin: Theme.space(2)
                            anchors.right: opensIcon.visible ? opensIcon.left : parent.right
                            anchors.rightMargin: Theme.space(2)
                            y: Theme.space(2)
                            spacing: Theme.space(0.5)

                            Text {
                                width: parent.width
                                text: resultButton.modelData.title
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.Medium
                            }
                            Text {
                                width: parent.width
                                text: resultButton.modelData.detail
                                elide: Text.ElideRight
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                        // Rows that open a dialog of their own (Media & cache, the shortcuts editor...).
                        Icon {
                            id: opensIcon
                            visible: resultButton.opensDialog
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.space(2)
                            y: Theme.space(2)
                            name: "external-link"
                            size: Theme.iconXs
                            color: Theme.textTertiary
                        }
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
            Text {
                objectName: "settingsNoResults"
                anchors.top: searchField.bottom
                anchors.topMargin: Theme.space(4)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(4)
                anchors.rightMargin: Theme.space(4)
                visible: resultList.visible && root.results.length === 0
                //: Settings search without a match; %1 = what was typed
                text: qsTr("No setting matches “%1”.").arg(root.searchText.trim())
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            Column {
                id: categoryList
                anchors.top: searchField.bottom
                anchors.topMargin: Theme.space(3)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                visible: !resultList.visible
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
        InterfaceSettingsPage {}
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
                    rowKey: "voiceOver"
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
                    rowKey: "audioPlugins"
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
                    rowKey: "shortcuts"
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
                    rowKey: "trainer"
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
                    rowKey: "extensions"
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
                    rowKey: "pluginFolders"
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
