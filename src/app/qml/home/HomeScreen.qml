pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Home screen (M5f; design language v3): shown when the app starts and from File > Home, over the editor. A rail
// on the left (HomeNav: the logo, the page's sections, Settings), a header (a greeting, a search over the projects,
// Open… and New project, Back, the window buttons) and one page of islands:
//   Continue           the project opened last, large, with Open (ContinueCard).
//   One-click import   a drop zone (files dropped anywhere here) and Premiere Pro / DaVinci Resolve / Final Cut Pro
//                      buttons, each saying what to export there. Files are checked first (ProjectFile.detectImport):
//                      timelines import into a new project whose first imported sequence opens, with the import
//                      report; a .prproj waits for the experimental reader to be chosen; files Wabcut cannot read say
//                      what to export instead.
//   Learn              local documents (import, keyboard shortcuts, what's new).
//   New gaming video   a game recording becomes a project matching it, each audio stream on its own named track
//                      (GamingVideoCard, M5o); a single video dropped anywhere here goes to it.
//   New project        sequence preset, name and folder (NewProjectCard).
//   Projects           recent projects as a grid or a list, all or starred, filtered by the search (RecentProjects).
// The editor's project-file flow (unsaved changes first) lives in Main: Home only asks for it with signals. While Main
// opens a project it sets `busyText`, and a cover with the echo mark's chase loader says so.
// Cards rise in one after another when it opens; under reduce-motion everything simply appears.
FocusScope {
    id: root
    objectName: "homeScreen"

    property bool opened: false
    /// Something is open behind Home (a project, or the demo): "Back" and Esc return to it.
    property bool canReturn: true
    /// A wait in progress ("Opening project…"): a cover with the chase loader; "" for none. Main sets it.
    property string busyText: ""
    readonly property bool customFrame: !Preferences.nativeFrame
    readonly property bool maximized: Window.window !== null
                                      && (Window.window.visibility === Window.Maximized
                                          || Window.window.visibility === Window.FullScreen)
    readonly property alias importCard: importCard
    readonly property alias newProjectCard: newProjectCard
    readonly property alias gamingCard: gamingCard
    readonly property alias recentCard: recentCard
    readonly property alias continueCard: continueCard
    /// The files dragged over Home are one game recording (they go to the gaming card, not the import).
    property bool dragRecording: false
    /// The header's search text (it narrows the projects).
    readonly property alias query: search.text
    /// The section in view (HomeNav lights it): "home", "projects", "import", "gaming" or "learn".
    property string currentSection: "home"
    readonly property var sections: [
        { key: "home", label: qsTr("Home"), hue: "violet" },
        { key: "projects", label: qsTr("Projects"), hue: "blue" },
        { key: "import", label: qsTr("Import"), hue: "mint" },
        { key: "gaming", label: qsTr("Gaming"), hue: "pink" },
        { key: "learn", label: qsTr("Learn"), hue: "orange" }
    ].filter(s => s.key !== "learn" || Home.links.length > 0)
    /// The hour the greeting was chosen at (read when Home opens).
    property int hour: new Date().getHours()
    readonly property string greeting: hour < 5 || hour >= 18 ? qsTr("Good evening")
                                     : hour < 12 ? qsTr("Good morning") : qsTr("Good afternoon")
    // A scroll started by the rail: the section stays as chosen until it ends.
    property bool navigating: false

    signal createRequested(string name, url folder, var sequence)
    signal gamingRequested(string name, url folder, var tracks) // GamingStart.create() after the unsaved-changes prompt
    signal openRequested(url file)                  // an empty url: choose a file
    signal importRequested(var urls, bool experimental)
    signal settingsRequested()

    function open() {
        Home.refreshRecent()
        importCard.pending = null
        importCard.guidance = ""
        importCard.error = ""
        newProjectCard.error = ""
        gamingCard.reset()
        search.text = ""
        hour = new Date().getHours()
        flick.contentY = 0
        currentSection = "home"
        opened = true
        forceActiveFocus()
    }
    function close() {
        opened = false
    }
    /// One game recording (a video file on its own): the gaming card reads it.
    function isRecording(urls) {
        return !!urls && urls.length === 1 && GamingStart.isRecording(urls[0])
               && ProjectFile.detectImport(urls[0]).kind === "media"
    }
    /// Files dropped or chosen: a project opens, one video goes to the gaming card, timelines and other media import
    /// into a new project; a .prproj waits for the experimental reader, files Wabcut cannot read say what to export
    /// instead.
    function handleUrls(urls) {
        if (!urls || urls.length === 0)
            return
        importCard.pending = null
        importCard.guidance = ""
        importCard.error = ""
        const info = ProjectFile.detectImport(urls[0])
        if (info.kind === "project") {
            openRequested(urls[0])
            return
        }
        if (isRecording(urls)) {
            gamingCard.inspect(urls[0])
            revealGamingCard()
            return
        }
        if (info.kind === "timeline" && info.experimental) {
            const path = urls[0].toString()
            importCard.pending = { url: urls[0], fileName: decodeURIComponent(path.substring(path.lastIndexOf("/") + 1)) }
            return
        }
        if (info.kind === "timeline" && !info.supported) {
            importCard.guidance = info.guidance !== "" ? info.guidance
                                                       : qsTr("Wabcut cannot read %1 files yet.").arg(info.formatName)
            return
        }
        importRequested(urls, false)
    }
    function acceptExperimental() {
        const pending = importCard.pending
        importCard.pending = null
        if (pending)
            importRequested([pending.url], true)
    }
    function showImportError(message) {
        importCard.error = message
    }
    function showCreateError(message) {
        newProjectCard.error = message
    }
    function showGamingError(message) {
        gamingCard.error = message
    }
    /// Scrolls the gaming card into view (one column: it sits below the import and Learn).
    function revealGamingCard() {
        root.reveal(gamingCard)
    }

    // ---- The page's sections ----
    function sectionItem(key: string): Item {
        return key === "projects" ? recentCard : key === "import" ? importCard : key === "gaming" ? gamingCard
             : key === "learn" ? learnCard : continueCard
    }
    /// An item's top in the page.
    function topOf(item: Item): real {
        return item.mapToItem(content, 0, 0).y
    }
    function maxContentY(): real {
        return Math.max(0, flick.contentHeight - flick.height)
    }
    /// Scrolls to a section (the rail): its top just under the header.
    function scrollTo(key: string) {
        const target = key === "home" ? 0 : Math.max(0, Math.min(root.topOf(root.sectionItem(key)) - Theme.gap,
                                                                  root.maxContentY()))
        root.currentSection = key
        scrollAnimation.stop()
        if (Theme.reduceMotion || Math.abs(target - flick.contentY) < 1) {
            flick.contentY = target
            return
        }
        root.navigating = true
        scrollAnimation.to = target
        scrollAnimation.start()
    }
    /// Scrolls just enough to show the top of `item` (and some of it) when it is out of view.
    function reveal(item: Item) {
        const top = root.topOf(item)
        if (top < flick.contentY || top + Theme.space(40) > flick.contentY + flick.height)
            flick.contentY = Math.max(0, Math.min(top - Theme.gap, root.maxContentY()))
    }
    /// The section whose top was scrolled past last; at the end of the page, the lowest one in view.
    function updateSection() {
        if (root.navigating || !root.opened)
            return
        const probe = flick.contentY + Theme.space(16)
        const atEnd = flick.contentHeight > flick.height && flick.contentY >= root.maxContentY() - 1
        let best = "home"
        let bestTop = -1
        for (const s of root.sections) {
            const item = root.sectionItem(s.key)
            if (!item || !item.visible)
                continue
            const top = s.key === "home" ? 0 : root.topOf(item)
            const reached = atEnd ? top < flick.contentY + flick.height : top <= probe
            if (reached && top > bestTop) {
                best = s.key
                bestTop = top
            }
        }
        root.currentSection = best
    }
    /// The header's "New project": to the card, the name ready to type.
    function startNewProject() {
        root.reveal(newProjectCard)
        newProjectCard.focusName()
    }

    visible: opened || opacity > 0
    opacity: opened ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
    enabled: opened
    // No editor shortcut fires behind Home.
    KeymapScope.suppressShortcuts: true

    Keys.onEscapePressed: event => {
        if (importCard.pending) {
            importCard.pending = null
            event.accepted = true
        } else if (root.canReturn) {
            root.close()
            event.accepted = true
        }
    }

    // The window moves by its empty header area (custom frame), double-click maximizes.
    component WindowDragSurface: Item {
        id: surface
        TapHandler {
            gesturePolicy: TapHandler.DragThreshold
            onDoubleTapped: {
                const w = surface.Window.window
                if (!w)
                    return
                if (w.visibility === Window.Maximized || w.visibility === Window.FullScreen)
                    w.showNormal()
                else
                    w.showMaximized()
            }
        }
        DragHandler {
            target: null
            onActiveChanged: {
                if (active && surface.Window.window)
                    surface.Window.window.startSystemMove()
            }
        }
    }

    // Backdrop: the black stage.
    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }

    // Files dropped anywhere on the Home screen.
    DropArea {
        id: drop
        objectName: "homeDropArea"
        anchors.fill: parent
        onEntered: drag => {
            root.dragRecording = drag.hasUrls && root.isRecording(drag.urls)
            if (!drag.hasUrls)
                drag.accepted = false
        }
        onDropped: drop => {
            root.dragRecording = false
            if (!drop.hasUrls)
                return
            drop.accept(Qt.CopyAction)
            root.handleUrls(drop.urls)
        }
    }

    HomeNav {
        id: nav
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.gap
        width: root.width >= Theme.space(325) ? Theme.space(55) : Theme.space(46)
        sections: root.sections
        current: root.currentSection
        dragsWindow: root.customFrame
        onActivated: key => root.scrollTo(key)
        onSettingsRequested: root.settingsRequested()
    }

    // Header: the greeting, search, Open…, New project, Back, the window controls; its empty area moves the window.
    Item {
        id: titleStrip
        anchors.left: nav.right
        anchors.leftMargin: Theme.gap
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.titleBarHeight + Theme.gap

        WindowDragSurface {
            anchors.fill: parent
            enabled: root.customFrame
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: Theme.gap
            anchors.leftMargin: Theme.space(1)
            anchors.rightMargin: Theme.gap
            spacing: Theme.space(2)

            Text {
                objectName: "homeGreeting"
                Layout.fillWidth: true
                Layout.minimumWidth: Theme.space(20)
                text: root.greeting
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textHero
                font.weight: Theme.weightTitle
                font.letterSpacing: -0.03 * Theme.textHero
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            TextField {
                id: search
                objectName: "homeSearch"
                Layout.fillWidth: true
                Layout.minimumWidth: Theme.space(30)
                Layout.preferredWidth: Theme.space(70)
                Layout.maximumWidth: Theme.space(70)
                implicitHeight: Theme.controlHeightLarge
                leftPadding: Theme.space(10)
                rightPadding: Theme.space(4)
                placeholderText: qsTr("Search projects")
                placeholderTextColor: Theme.textTertiary
                color: Theme.textPrimary
                selectionColor: Theme.accentFill
                selectedTextColor: Theme.accentOnFill
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                selectByMouse: true
                Accessible.name: qsTr("Search projects")
                // Esc clears the search first, then closes Home.
                Keys.onEscapePressed: event => {
                    if (search.text !== "") {
                        search.text = ""
                        event.accepted = true
                    } else {
                        event.accepted = false
                    }
                }
                onTextEdited: {
                    if (search.text.trim() !== "")
                        root.reveal(recentCard)
                }
                background: Rectangle {
                    radius: height / 2
                    color: Theme.panel
                    border.width: search.activeFocus ? Theme.focusRingWidth : 0
                    border.color: Theme.accentStroke
                }
                Icon {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(4)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: Theme.iconSm
                    color: Theme.textTertiary
                }
            }
            PillButton {
                objectName: "homeOpenButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                iconName: "folder-open"
                text: qsTr("Open…")
                tooltip: qsTr("Open a project file")
                command: "file.openProject"
                onClicked: root.openRequested("")
            }
            PillButton {
                objectName: "homeNewButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                variant: "primary"
                iconName: "plus"
                text: qsTr("New project")
                tooltip: qsTr("Choose a format and name for a new project")
                onClicked: root.startNewProject()
            }
            PillButton {
                objectName: "homeBackButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                visible: root.canReturn
                variant: "ghost"
                iconName: "arrow-right"
                text: ProjectFile.demo ? qsTr("Explore the demo project") : qsTr("Back to %1").arg(ProjectFile.name)
                tooltip: qsTr("Close Home (Esc)")
                onClicked: root.close()
            }
            Rectangle {
                visible: root.customFrame
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: Theme.hairline
                implicitHeight: Theme.controlHeightSmall - Theme.space(2)
                color: Theme.borderStrong
            }
            WindowControls {
                visible: root.customFrame
                Layout.alignment: Qt.AlignVCenter
                maximized: root.maximized
                onMinimizeRequested: root.Window.window?.showMinimized()
                onMaximizeRequested: {
                    const w = root.Window.window
                    if (w)
                        root.maximized ? w.showNormal() : w.showMaximized()
                }
                onCloseRequested: root.Window.window?.close()
            }
        }
    }

    Flickable {
        id: flick
        objectName: "homeContent"
        anchors.left: nav.right
        anchors.leftMargin: Theme.gap
        anchors.right: parent.right
        anchors.top: titleStrip.bottom
        anchors.topMargin: Theme.gap
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: content.implicitHeight + Theme.gap
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        onContentYChanged: root.updateSection()
        onContentHeightChanged: root.updateSection()

        NumberAnimation {
            id: scrollAnimation
            target: flick
            property: "contentY"
            duration: Theme.durCollapse
            easing.type: Easing.BezierSpline
            easing.bezierCurve: Theme.easeSnapOut
            onStopped: root.navigating = false
        }

        ColumnLayout {
            id: content
            width: flick.width - Theme.gap
            spacing: Theme.gap + Theme.space(1)

            GridLayout {
                id: grid
                Layout.fillWidth: true
                columns: width >= Theme.space(230) ? 2 : 1
                columnSpacing: content.spacing
                rowSpacing: content.spacing

                // Left: continue, import, learn
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: content.spacing

                    ContinueCard {
                        id: continueCard
                        Layout.fillWidth: true
                        order: 0
                        revealed: root.opened
                        onOpenRequested: file => root.openRequested(file)
                        onReturnRequested: root.close()
                    }

                    ImportCard {
                        id: importCard
                        Layout.fillWidth: true
                        order: 2
                        revealed: root.opened
                        dragActive: drop.containsDrag && !root.dragRecording
                        onChooseRequested: source => {
                            importDialog.nameFilters = source ? source.nameFilters : Home.timelineNameFilters
                            importDialog.title = source ? qsTr("Import from %1").arg(source.name) : qsTr("Import a timeline")
                            importDialog.open()
                        }
                        onExperimentalAccepted: root.acceptExperimental()
                        onPendingCancelled: importCard.pending = null
                    }

                    HomeCard {
                        id: learnCard
                        objectName: "homeLearnCard"
                        Layout.fillWidth: true
                        visible: Home.links.length > 0
                        order: 4
                        revealed: root.opened
                        title: qsTr("Learn")
                        hue: "orange"

                        Repeater {
                            model: Home.links
                            delegate: AbstractButton {
                                id: link
                                required property var modelData
                                objectName: "homeLink_" + modelData.id
                                Layout.fillWidth: true
                                implicitHeight: linkRow.implicitHeight + Theme.space(4)
                                focusPolicy: Qt.StrongFocus
                                hoverEnabled: true
                                Accessible.name: modelData.title
                                Accessible.description: modelData.detail
                                onClicked: Qt.openUrlExternally(modelData.url)

                                background: Rectangle {
                                    radius: Theme.radiusCard - Theme.space(0.5)
                                    color: link.hovered ? Theme.raised : "transparent"
                                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                                }
                                contentItem: RowLayout {
                                    id: linkRow
                                    spacing: Theme.space(2)
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: Theme.space(3)
                                        spacing: 0
                                        Text {
                                            Layout.fillWidth: true
                                            text: link.modelData.title
                                            elide: Text.ElideRight
                                            color: Theme.textPrimary
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.textMd
                                            font.weight: Theme.weightLabel
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: link.modelData.detail
                                            elide: Text.ElideRight
                                            color: Theme.textTertiary
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.textSm
                                        }
                                    }
                                    Icon {
                                        Layout.rightMargin: Theme.space(3)
                                        name: "external-link"
                                        size: Theme.iconSm
                                        color: link.hovered ? Theme.accentStroke : Theme.textTertiary
                                    }
                                }
                                FocusRing {
                                    targetRadius: Theme.radiusCard - Theme.space(0.5)
                                    visible: link.visualFocus
                                }
                                HoverHandler {
                                    enabled: link.enabled
                                    cursorShape: Qt.PointingHandCursor
                                }
                            }
                        }
                    }
                }

                // Right: new gaming video, new project
                ColumnLayout {
                    Layout.fillWidth: grid.columns === 1
                    Layout.preferredWidth: grid.columns === 1 ? -1 : Math.min(Theme.space(112), grid.width * 0.38)
                    Layout.alignment: Qt.AlignTop
                    spacing: content.spacing

                    GamingVideoCard {
                        id: gamingCard
                        Layout.fillWidth: true
                        order: 1
                        revealed: root.opened
                        dragActive: drop.containsDrag && root.dragRecording
                        onGamingRequested: (name, folder, tracks) => root.gamingRequested(name, folder, tracks)
                    }

                    NewProjectCard {
                        id: newProjectCard
                        Layout.fillWidth: true
                        order: 3
                        revealed: root.opened
                        onCreateRequested: (name, folder, sequence) => root.createRequested(name, folder, sequence)
                    }
                }
            }

            RecentProjects {
                id: recentCard
                Layout.fillWidth: true
                order: 5
                revealed: root.opened
                query: search.text
                onOpenRequested: file => root.openRequested(file)
            }
        }
    }

    // A wait (Main opening a project): the chase loader over Home, nothing clickable under it.
    Rectangle {
        id: busyCover
        objectName: "homeBusy"
        anchors.fill: parent
        z: 10
        visible: root.busyText !== ""
        color: Theme.scrim

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
        }
        LoaderPill {
            anchors.centerIn: parent
            variant: "chase"
            text: root.busyText
            running: busyCover.visible
        }
    }

    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        currentFolder: ProjectFile.folder
        onAccepted: root.handleUrls([selectedFile])
    }
}
