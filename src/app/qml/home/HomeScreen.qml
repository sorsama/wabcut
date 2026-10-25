pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The Home screen (M5f; design language v3; UX rework 2026-10-23: a start window with pages): shown when the app starts,
// from File > Home and File > New Project, over the editor. A fixed frame - nothing scrolls the whole window; lists
// scroll inside their own area:
//   HomeNav     the logo (moves the window), the pages Home / Projects / Learn, the version and Settings.
//   Header      the page's title (the greeting on Home; "New project" with a back arrow), the search over the projects
//               (typing opens Projects), Open..., Import..., New project, "Back to <project>" (when a real project is
//               behind Home, or the sample project Home was opened from with File > Home or File > New Project) and the
//               window buttons.
//   Strips      under the header: the experimental .prproj prompt, guidance for files Wabcut cannot read, and Home's
//               messages (errors, notes) - the editor's toast is not visible over Home.
//   Pages       `page`: "home" (HomePage: Continue, Start, Recent), "projects" (RecentProjects), "learn" (LearnPage),
//               "new" (NewProjectPage). They cross-fade (Theme.durState; at once under reduce motion); all but Home are
//               made the first time they show.
// The all-in-one Import: Import... (several files), Import folder..., a drop anywhere (the Home page's drop well lights;
// on other pages a full-window "Drop to import" shows) all go through Home.classifyImport: a project opens, timelines
// import into a new project, a .prproj waits for the experimental reader, unreadable files get guidance, and media
// open the New project page in "Start with media" mode.
// Keys (Home-local; the editor's shortcuts are suppressed here): Ctrl+N / Ctrl+Alt+N New project, Ctrl+O Open...,
// Ctrl+I Import..., Ctrl+F search, Ctrl+, (Cmd+,) Settings; Esc cancels a pending prompt, else leaves the New project
// page (back to the editor when File > New Project opened it), else closes Home when it can go back (`canReturn`: a
// real project behind it, or Home was opened from the editor - never into the sample nobody asked for at launch or
// after Close Project). Leaving the New project page for another page (the nav, the search) keeps what it holds
// (the media of an import, the name); Cancel, Esc and the back arrow drop it. The project-file flow (unsaved changes
// first) lives in Main: Home asks with signals. While Main works it sets `busyText` (runBusy), and a cover with the
// echo mark's chase loader says so.
FocusScope {
    id: root
    objectName: "homeScreen"

    property bool opened: false
    /// A wait in progress ("Opening project"): a cover with the chase loader; "" for none (runBusy sets it).
    property string busyText: ""
    readonly property bool customFrame: !Preferences.nativeFrame
    readonly property bool maximized: Window.window !== null
                                      && (Window.window.visibility === Window.Maximized
                                          || Window.window.visibility === Window.FullScreen)
    /// The page shown: "home", "projects", "learn" or "new".
    property string page: "home"
    /// Where the New project page goes back to.
    property string returnPage: "home"
    /// The header's search text (it narrows the projects).
    readonly property alias query: search.text
    /// The hour the greeting was chosen at (read when Home opens).
    property int hour: new Date().getHours()
    readonly property string greeting: hour < 5 || hour >= 18 ? qsTr("Good evening")
                                     : hour < 12 ? qsTr("Good morning") : qsTr("Good afternoon")
    /// Home was opened from the editor (File > Home, File > New Project: openFromEditor), not at launch or after Close
    /// Project.
    property bool fromEditor: false
    /// Back and Esc return to the project behind Home: a real one, or the sample project the user was in (opened from
    /// the editor) - never the sample nobody asked for.
    readonly property bool canReturn: !ProjectFile.demo || root.fromEditor
    /// Leaving the New project page for another page (the nav, the search) keeps what it holds for when it shows again.
    property bool newPageKept: false
    /// Files are dragged over Home.
    readonly property bool dragging: drop.containsDrag
    /// A .prproj waiting for the experimental reader: { url, name, rest: [urls], deferred: [urls] }, or null.
    property var pending: null
    /// Home's message strip ("" hides it) and its tone ("info", "warning", "danger").
    property string message: ""
    property string messageTone: "info"
    /// The strip's action pill ("" none) and what it runs (showMessage's options: the editor's Save As…, Show in...).
    property string messageActionText: ""
    property var messageAction: null
    /// Guidance for files Wabcut cannot read ("" hides it).
    property string importMessage: ""
    /// What an import that closes Home left out (notes); Main shows it in the editor once Home has closed.
    property string closingNote: ""
    /// The short-lived note at the bottom (a removal with Undo, a copied path); "" hides it.
    property string status: ""
    property bool statusUndo: false
    // A starred project Remove from List unstarred: Undo stars it again.
    property string removedStar: ""
    // The missing project Locate... is for.
    property var locating: null
    readonly property var pages: [
        { key: "home", label: qsTr("Home"), hue: "violet" },
        { key: "projects", label: qsTr("Projects"), hue: "blue" },
        { key: "learn", label: qsTr("Learn"), hue: "orange" }
    ]
    readonly property string pageTitle: root.page === "projects" ? qsTr("Projects") : root.page === "learn" ? qsTr("Learn")
                                      : root.page === "new" ? qsTr("New project") : root.greeting
    /// The header has little room: Open... and Import... show their icons only.
    readonly property bool compactHeader: titleStrip.width < Theme.space(250)

    signal openRequested(url file)                  // an empty url: choose a file (Main's dialog)
    signal openAlongsideRequested(url file)
    signal createRequested(string name, url folder, var settings)
    signal createFromMediaRequested(string name, url folder, var settings, var files, var options)
    signal importRequested(var urls, bool experimental, var deferred)
    signal sampleRequested()
    signal helpRequested(string what)
    signal settingsRequested()

    function open() {
        Home.refreshRecent()
        GamingStart.clear() // "Start with media" starts afresh
        root.pending = null
        root.message = ""
        root.importMessage = ""
        root.status = ""
        root.closingNote = ""
        root.fromEditor = false
        root.newPageKept = false
        search.text = ""
        root.hour = new Date().getHours()
        root.page = "home"
        root.opened = true
        root.forceActiveFocus()
    }
    function close() {
        root.opened = false
    }
    /// Opens Home (when closed) on `key` ("home", "projects", "learn" or "new"; "new" starts a fresh project) with the
    /// keyboard focus in the page.
    function openPage(key: string) {
        if (!root.opened)
            root.open()
        root.showPage(key)
        Qt.callLater(root.focusPage)
    }
    /// File > Home and File > New Project (Main): Home over the editor the user was in, so Back and Esc go back to it -
    /// even to the sample project - and the New project page opened this way goes back to the editor.
    function openFromEditor(key: string) {
        const wasOpen = root.opened
        root.openPage(key)
        if (wasOpen)
            return
        root.fromEditor = true
        if (root.page === "new")
            root.returnPage = "editor"
    }
    /// Switches pages, leaving the focus where it is (the nav, the search).
    function showPage(key: string) {
        const next = key === "projects" || key === "learn" || key === "new" ? key : "home"
        if (next === "new" && root.page !== "new") {
            root.returnPage = root.page
            // A fresh project, unless the page was only left for another one (the media of an import stay).
            if (!root.newPageKept)
                root.newPage().reset()
            root.newPageKept = false
        } else if (root.page === "new" && next !== "new") {
            root.newPageKept = true
        }
        root.page = next
    }
    /// The New project page (made on first use).
    function newPage(): NewProjectPage {
        newSlot.created = true
        return newSlot.item as NewProjectPage
    }
    /// The shown page's first control takes the keyboard focus.
    function focusPage() {
        if (root.page === "new")
            root.newPage().focusName()
        else if (root.page === "projects" && projectsSlot.item)
            (projectsSlot.item as RecentProjects).focusList()
        else if (root.page === "learn" && learnSlot.item)
            (learnSlot.item as LearnPage).focusFirst()
        else if (homeSlot.item)
            (homeSlot.item as HomePage).focusFirst()
    }
    /// Back from the New project page (Esc, Cancel, the back arrow): what it held is dropped. Opened by File > New
    /// Project, it goes back to the editor; else to the page it was opened from.
    function leaveNewPage() {
        GamingStart.clear()
        root.newPageKept = false
        if (root.returnPage === "editor" && root.canReturn) {
            root.close()
            return
        }
        root.page = root.returnPage === "new" || root.returnPage === "editor" ? "home" : root.returnPage
        Qt.callLater(root.focusPage)
    }
    /// Main takes what the import that asked to open or import left out (with the request: a note never outlives it).
    function takeClosingNote(): string {
        const note = root.closingNote
        root.closingNote = ""
        return note
    }
    /// A message for Home's strip (errors and guidance from Home or the editor: `home.showMessage(text)`). `tone`:
    /// "warning" (default), "danger" or "info".
    function showMessage(text, tone) {
        root.message = text ?? ""
        root.messageTone = tone === "danger" || tone === "info" ? tone : "warning"
        root.messageActionText = ""
        root.messageAction = null
    }
    /// showMessage with the toast's `options` (UX rework, Main.notify: { actionText, action }): an action pill on the
    /// strip that closes it and runs `action` (the editor's Save As…, Show in Explorer).
    function showMessageWithAction(text, tone, options) {
        root.showMessage(text, tone)
        const action = options ? options.action : null
        if (typeof action === "function" && (options.actionText ?? "") !== "") {
            root.messageAction = action
            root.messageActionText = options.actionText
        }
    }
    /// Before the UX rework: Main's open and import errors.
    function showImportError(text) {
        root.showMessage(text, "danger")
    }
    /// The New project page's error (Main: the backend refused).
    function showCreateError(text) {
        if (root.page === "new")
            root.newPage().error = text
        else
            root.showMessage(text, "danger")
    }
    /// Shows the busy cover with `text`, then runs `work` a frame or two later (the work is synchronous).
    function runBusy(text: string, work: var) {
        root.busyText = text
        busyLater.work = work
        busyLater.restart()
    }
    function showStatus(text: string, undo: bool) {
        root.status = text
        root.statusUndo = undo
        statusTimer.restart()
    }
    function chooseProject() {
        openDialog.open()
    }
    function chooseImport() {
        importDialog.open()
    }
    function chooseImportFolder() {
        importFolderDialog.open()
    }
    function urlsOf(list: var): var {
        return (list ?? []).map(f => f.url)
    }

    // ---- The all-in-one Import ----
    /// Files dropped or chosen (several; folders are expanded): Home.classifyImport decides what happens.
    function handleUrls(urls) {
        if (!urls || urls.length === 0)
            return
        root.pending = null
        root.importMessage = ""
        root.message = ""
        root.closingNote = ""
        const c = Home.classifyImport(urls)
        const notes = c.notes ?? []
        if (notes.length > 0)
            root.showMessage(notes.join("\n"), "info")
        switch (c.route) {
        case "project":
            root.closingNote = notes.join("\n")
            root.openRequested(c.projects[0].url)
            break
        case "timelines":
            root.closingNote = notes.join("\n")
            root.importRequested(root.urlsOf(c.timelines).concat(root.urlsOf(c.media), root.urlsOf(c.captions)), false,
                                 root.urlsOf(c.lottie).concat(root.urlsOf(c.layered)))
            break
        case "experimental":
            root.pending = {
                url: c.experimental[0].url, name: c.experimental[0].name,
                rest: root.urlsOf(c.media).concat(root.urlsOf(c.captions)),
                deferred: root.urlsOf(c.lottie).concat(root.urlsOf(c.layered))
            }
            break
        case "media":
            root.showPage("new")
            root.newPage().startWithMedia(c)
            Qt.callLater(root.focusPage)
            break
        case "unsupported":
            root.importMessage = c.message ?? ""
            break
        default:
            root.showMessage(c.message !== undefined && c.message !== "" ? c.message : qsTr("There is nothing to import."))
        }
    }
    /// The .prproj prompt's Import: the experimental reader, with the media that came along.
    function acceptExperimental() {
        const p = root.pending
        root.pending = null
        if (p)
            root.importRequested([p.url].concat(p.rest), true, p.deferred)
    }

    // ---- Projects ----
    function samePath(a: string, b: string): bool {
        const norm = s => String(s).replace(/\\/g, "/").toLowerCase()
        return norm(a) === norm(b)
    }
    /// The project is the one open behind Home.
    function isCurrent(project: var): bool {
        return project !== null && ProjectFile.filePath !== "" && root.samePath(project.path, ProjectFile.filePath)
    }
    function openProject(project: var) {
        if (!project || project.missing)
            return
        if (root.isCurrent(project))
            root.close()
        else
            root.openRequested(project.url)
    }
    function showProjectMenu(project: var, item: Item, x: real, y: real) {
        projectMenu.openFor(project, item, x, y, root.isCurrent(project))
    }
    function removeProject(project: var) {
        const starred = Preferences.isStarred(project.path)
        if (starred)
            Preferences.setStarred(project.path, false)
        root.removedStar = starred ? project.path : ""
        Home.removeRecent(project.url)
        root.showStatus(qsTr("Removed %1 from the list").arg(project.name), true)
    }
    function undoRemove() {
        Home.undoRemoveRecent()
        if (root.removedStar !== "")
            Preferences.setStarred(root.removedStar, true)
        root.removedStar = ""
        root.status = ""
    }
    function projectAction(action: string, project: var) {
        switch (action) {
        case "open":
            root.openProject(project)
            break
        case "openAlongside":
            root.openAlongsideRequested(project.url)
            break
        case "reveal":
            if (!Home.revealProject(project.url))
                root.showMessage(qsTr("%1 cannot be shown in its folder.").arg(project.name))
            break
        case "copyPath":
            Home.copyPath(project.url)
            root.showStatus(qsTr("Copied the path of %1").arg(project.name), false)
            break
        case "star":
            Preferences.setStarred(project.path, !Preferences.isStarred(project.path))
            break
        case "locate":
            root.locating = project
            locateDialog.currentFolder = Home.existingFolder(project.url)
            locateDialog.open()
            break
        case "remove":
            root.removeProject(project)
            break
        }
    }
    function locate(file: url) {
        const project = root.locating
        root.locating = null
        if (!project)
            return
        const r = Home.locateProject(project.url, file)
        if (!r.ok) {
            root.showMessage(r.error, "danger")
            return
        }
        if (Preferences.isStarred(r.oldPath)) {
            Preferences.setStarred(r.oldPath, false)
            Preferences.setStarred(r.path, true)
        }
        root.showStatus(qsTr("%1 is found again").arg(r.name), false)
    }
    /// How Home's key with `letter` reads on this platform ("Ctrl+N", "⌘N").
    function keyText(letter: string): string {
        return Qt.platform.os === "osx" ? "⌘" + letter : qsTr("Ctrl+%1").arg(letter)
    }
    function shortName(name: string): string {
        return name.length > 28 ? name.slice(0, 26) + "…" : name
    }

    visible: opened || opacity > 0
    opacity: opened ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
    enabled: opened
    // No editor shortcut fires behind Home.
    KeymapScope.suppressShortcuts: true

    Keys.onEscapePressed: event => {
        if (root.pending) {
            root.pending = null
            event.accepted = true
        } else if (root.page === "new") {
            root.leaveNewPage()
            event.accepted = true
        } else if (root.canReturn) {
            root.close()
            event.accepted = true
        }
    }

    // Home's own keys (the editor's are suppressed here); they reach Home through the focus chain, so a text field
    // keeps its own (Ctrl+Z undoes typing there) and a menu or dialog keeps every key.
    Keys.onPressed: event => {
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        if (!ctrl || (event.modifiers & Qt.ShiftModifier) || root.busyText !== "")
            return
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        if (event.key === Qt.Key_N) {
            root.openPage("new")
        } else if (alt) {
            return
        } else if (event.key === Qt.Key_O) {
            root.chooseProject()
        } else if (event.key === Qt.Key_I) {
            root.chooseImport()
        } else if (event.key === Qt.Key_F) {
            search.forceActiveFocus(Qt.ShortcutFocusReason)
            search.selectAll()
        } else if (event.key === Qt.Key_Comma) {
            // Settings (Cmd+, on macOS), as the editor's Edit > Settings... (the keymap's edit.preferences is
            // suppressed here).
            root.settingsRequested()
        } else if (event.key === Qt.Key_Z && root.status !== "" && root.statusUndo) {
            root.undoRemove()
        } else {
            return
        }
        event.accepted = true
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

    // A page: made when it first shows (Home at once), kept afterwards; pages cross-fade.
    component PageSlot: Loader {
        id: slot
        required property string key
        property bool created: false
        readonly property bool shown: root.page === slot.key
        anchors.fill: parent
        active: slot.created || slot.shown
        visible: opacity > 0
        opacity: slot.shown ? 1 : 0
        enabled: slot.shown
        Behavior on opacity { NumberAnimation { duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft } }
        onShownChanged: {
            if (slot.shown)
                slot.created = true
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
            if (!drag.hasUrls)
                drag.accepted = false
        }
        onDropped: drop => {
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
        sections: root.pages
        current: root.page
        dragsWindow: root.customFrame
        onActivated: key => root.showPage(key)
        onSettingsRequested: root.settingsRequested()
    }

    // Header: the page's title, search, Open..., Import..., New project, Back, the window controls; its empty area
    // moves the window.
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

            IconButton {
                objectName: "homeNewBack"
                visible: root.page === "new"
                variant: "secondary"
                iconName: "chevron-left"
                tooltip: qsTr("Back (Esc)")
                onClicked: root.leaveNewPage()
            }
            Text {
                objectName: root.page === "home" ? "homeGreeting" : "homePageTitle"
                Layout.fillWidth: true
                Layout.minimumWidth: Theme.space(20)
                text: root.pageTitle
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: root.page === "home" ? Theme.textHero : Theme.textDisplay + 4
                font.weight: Theme.weightTitle
                font.letterSpacing: -0.8
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            TextField {
                id: search
                objectName: "homeSearch"
                Layout.preferredWidth: root.compactHeader ? Theme.space(44) : Theme.space(64)
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
                // Esc clears the search first, then does what Esc does on Home.
                Keys.onEscapePressed: event => {
                    if (search.text !== "") {
                        search.text = ""
                        event.accepted = true
                    } else {
                        event.accepted = false
                    }
                }
                Keys.onDownPressed: Qt.callLater(root.focusPage)
                // Typing on any page shows the matching projects.
                onTextEdited: {
                    if (search.text.trim() !== "" && root.page !== "projects")
                        root.showPage("projects")
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
                variant: Theme.dark ? "secondary" : "ghost" // a raised pill would not show on the light stage
                iconName: "folder-open"
                text: root.compactHeader ? "" : qsTr("Open…")
                tooltip: qsTr("Open a project file")
                shortcut: root.keyText("O")
                onClicked: root.chooseProject()
            }
            PillButton {
                objectName: "homeImportButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                variant: Theme.dark ? "secondary" : "ghost"
                iconName: "import"
                text: root.compactHeader ? "" : qsTr("Import…")
                tooltip: qsTr("Import media, a game recording, timelines or a project")
                shortcut: root.keyText("I")
                onClicked: root.chooseImport()
            }
            PillButton {
                objectName: "homeNewButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                visible: root.page !== "new"
                variant: "primary"
                iconName: "plus"
                text: qsTr("New project")
                tooltip: qsTr("Choose a format and a name for a new project")
                shortcut: root.keyText("N")
                onClicked: root.openPage("new")
            }
            PillButton {
                objectName: "homeBackButton"
                Layout.preferredHeight: Theme.controlHeightLarge
                visible: root.canReturn
                variant: "ghost"
                iconName: "arrow-right"
                readonly property string projectName: ProjectFile.demo ? qsTr("the sample project") : ProjectFile.name
                text: root.compactHeader ? qsTr("Back") : qsTr("Back to %1").arg(root.shortName(projectName))
                tooltip: qsTr("Close Home and go back to %1 (Esc)").arg(projectName)
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

    // Strips under the header: the .prproj prompt, guidance for unreadable files, Home's messages.
    ColumnLayout {
        id: strips
        anchors.left: nav.right
        anchors.leftMargin: Theme.gap
        anchors.right: parent.right
        anchors.rightMargin: Theme.gap
        anchors.top: titleStrip.bottom
        anchors.topMargin: visible ? Theme.gap : 0
        // (Not from the strips' own `visible`: a hidden parent hides them.)
        visible: root.pending !== null || root.importMessage !== "" || root.message !== ""
        spacing: Theme.space(2)

        HomeMessageStrip {
            id: experimentalStrip
            objectName: "homeExperimentalPrompt"
            Layout.fillWidth: true
            visible: root.pending !== null
            tone: "warning"
            closable: false
            title: qsTr("%1 · Premiere Pro project").arg(root.pending ? root.pending.name : "")
            message: qsTr("Premiere projects are read by an experimental reader: some effects may not come across. The import report lists what did.")

            PillButton {
                objectName: "homeExperimentalCancel"
                compact: true
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: root.pending = null
            }
            PillButton {
                objectName: "homeExperimentalImport"
                compact: true
                variant: "primary"
                text: qsTr("Import with the experimental reader")
                onClicked: root.acceptExperimental()
            }
        }
        HomeMessageStrip {
            id: importStrip
            objectName: "homeImportMessage"
            Layout.fillWidth: true
            visible: root.importMessage !== "" && root.pending === null
            tone: "info"
            message: root.importMessage
            onCloseRequested: root.importMessage = ""

            HomeLink {
                objectName: "homeImportHowTo"
                iconName: "external-link"
                text: qsTr("How to export")
                tooltip: qsTr("The guide to moving timelines between editors (website)")
                onClicked: Qt.openUrlExternally(Home.guideUrl("exchange-timelines"))
            }
        }
        HomeMessageStrip {
            id: messageStrip
            objectName: "homeMessage"
            Layout.fillWidth: true
            visible: root.message !== ""
            tone: root.messageTone
            message: root.message
            onCloseRequested: root.message = ""

            PillButton {
                objectName: "homeMessageAction"
                visible: root.messageActionText !== ""
                compact: true
                variant: "primary"
                text: root.messageActionText
                onClicked: {
                    const run = root.messageAction
                    root.message = ""
                    root.messageActionText = ""
                    root.messageAction = null
                    if (typeof run === "function")
                        run()
                }
            }
        }
    }

    // The pages.
    Item {
        id: pageArea
        objectName: "homePages"
        anchors.left: nav.right
        anchors.leftMargin: Theme.gap
        anchors.right: parent.right
        anchors.rightMargin: Theme.gap
        anchors.top: strips.visible ? strips.bottom : titleStrip.bottom
        anchors.topMargin: Theme.gap
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.gap

        PageSlot {
            id: homeSlot
            key: "home"
            created: true
            sourceComponent: HomePage {
                revealed: root.opened
                dragActive: root.dragging
                newKeys: root.keyText("N")
                openKeys: root.keyText("O")
                importKeys: root.keyText("I")
                onOpenRequested: file => root.openRequested(file)
                onReturnRequested: root.close()
                onRevealRequested: file => {
                    if (!Home.revealProject(file))
                        root.showMessage(qsTr("The project cannot be shown in its folder."))
                }
                onNewProjectRequested: root.openPage("new")
                onChooseProjectRequested: root.chooseProject()
                onImportRequested: root.chooseImport()
                onImportFolderRequested: root.chooseImportFolder()
                onAllProjectsRequested: root.openPage("projects")
                onLearnRequested: root.openPage("learn")
                onProjectOpenRequested: project => root.openProject(project)
                onProjectMenuRequested: (project, item, x, y) => root.showProjectMenu(project, item, x, y)
                onProjectRemoveRequested: project => root.removeProject(project)
            }
        }
        PageSlot {
            id: projectsSlot
            key: "projects"
            sourceComponent: RecentProjects {
                revealed: root.opened
                query: search.text
                onOpenRequested: project => root.openProject(project)
                onMenuRequested: (project, item, x, y) => root.showProjectMenu(project, item, x, y)
                onRemoveRequested: project => root.removeProject(project)
                onClearUnstarredRequested: clearConfirm.open()
                onNewProjectRequested: root.openPage("new")
                onChooseProjectRequested: root.chooseProject()
            }
        }
        PageSlot {
            id: learnSlot
            key: "learn"
            sourceComponent: LearnPage {
                revealed: root.opened
                onSampleRequested: root.sampleRequested()
                onHelpRequested: what => root.helpRequested(what)
            }
        }
        PageSlot {
            id: newSlot
            key: "new"
            sourceComponent: NewProjectPage {
                revealed: root.opened
                onCreateRequested: (name, folder, settings) => root.createRequested(name, folder, settings)
                onCreateFromMediaRequested: (name, folder, settings, files, options) =>
                                            root.createFromMediaRequested(name, folder, settings, files, options)
                onCancelRequested: root.leaveNewPage()
            }
        }

        // Dragging files over a page other than Home: the whole page says where they go.
        Rectangle {
            objectName: "homeDropOverlay"
            anchors.fill: parent
            visible: root.dragging && root.page !== "home"
            z: 5
            radius: Theme.radiusPanel
            color: Qt.alpha(Theme.canvas, 0.82)
            border.width: Theme.focusRingWidth
            border.color: Theme.accentStroke

            Column {
                anchors.centerIn: parent
                spacing: Theme.space(2)
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Theme.iconXl + Theme.space(6)
                    height: width
                    radius: width / 2
                    color: Theme.accentFill
                    Icon {
                        anchors.centerIn: parent
                        name: "download"
                        size: Theme.iconXl
                        color: Theme.accentOnFill
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Drop to import")
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.textDisplay
                    font.weight: Theme.weightTitle
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Media, game recordings, timelines or projects")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
            }
        }

        // A short note at the bottom: a removal (with Undo) or a copied path.
        Rectangle {
            id: statusStrip
            objectName: "homeUndoStrip"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.space(4)
            z: 6
            visible: root.status !== ""
            width: Math.min(parent.width - Theme.space(8), statusRow.implicitWidth + Theme.space(8))
            height: Theme.controlHeightLarge + Theme.space(2)
            radius: height / 2
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.borderStrong
            Accessible.role: Accessible.AlertMessage
            Accessible.name: root.status

            RowLayout {
                id: statusRow
                anchors.fill: parent
                anchors.leftMargin: Theme.space(5)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(3)
                Text {
                    Layout.fillWidth: true
                    text: root.status
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
                PillButton {
                    objectName: "homeUndoButton"
                    visible: root.statusUndo
                    compact: true
                    variant: "primary"
                    iconName: "undo-2"
                    text: qsTr("Undo")
                    tooltip: qsTr("Put it back in the list")
                    shortcut: root.keyText("Z")
                    onClicked: root.undoRemove()
                }
                IconButton {
                    compact: true
                    iconName: "x"
                    iconSize: Theme.iconXs
                    tooltip: qsTr("Dismiss")
                    onClicked: root.status = ""
                }
            }
        }
        Timer {
            id: statusTimer
            interval: 5000
            onTriggered: root.status = ""
        }
    }

    // A wait (Main opening or creating a project): the chase loader over Home, nothing clickable under it.
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
    Timer {
        id: busyLater
        property var work: null
        interval: 48
        onTriggered: {
            const work = busyLater.work
            busyLater.work = null
            if (work)
                work()
            root.busyText = ""
        }
    }

    ProjectMenu {
        id: projectMenu
        onChosen: (action, project) => root.projectAction(action, project)
    }
    WabcutDialog {
        id: clearConfirm
        objectName: "homeClearConfirm"
        title: qsTr("Clear unstarred projects?")

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Every project that is not starred leaves the list. The files stay where they are.")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        footerData: [
            PillButton {
                objectName: "homeClearCancel"
                text: qsTr("Cancel")
                onClicked: clearConfirm.close()
            },
            PillButton {
                objectName: "homeClearAccept"
                variant: "primary"
                text: qsTr("Clear")
                onClicked: {
                    clearConfirm.close()
                    const n = Home.clearUnstarred(Preferences.starredProjects())
                    root.removedStar = ""
                    if (n > 0)
                        root.showStatus(qsTr("Cleared %n project(s)", "", n), true)
                }
            }
        ]
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open a project")
        fileMode: FileDialog.OpenFile
        nameFilters: Home.projectNameFilters
        currentFolder: Home.defaultFolder
        onAccepted: root.openRequested(selectedFile)
    }
    // 0.4: both import dialogs open where the last dialog import came from (ProjectFile.importFolder) and remember
    // their choice for the next one (the editor's Import… and Import Folder… share it).
    FileDialog {
        id: importDialog
        title: qsTr("Import")
        fileMode: FileDialog.OpenFiles
        nameFilters: Home.importNameFilters
        currentFolder: ProjectFile.importFolder
        onAccepted: {
            ProjectFile.rememberImportFolder(selectedFiles)
            root.handleUrls(selectedFiles)
        }
    }
    FolderDialog {
        id: importFolderDialog
        title: qsTr("Import a folder")
        currentFolder: ProjectFile.importFolder
        onAccepted: {
            ProjectFile.rememberImportFolder([selectedFolder])
            root.handleUrls([selectedFolder])
        }
    }
    FileDialog {
        id: locateDialog
        title: qsTr("Locate %1").arg(root.locating ? root.locating.name : "")
        fileMode: FileDialog.OpenFile
        nameFilters: Home.projectNameFilters
        onAccepted: root.locate(selectedFile)
        onRejected: root.locating = null
    }
}
