pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Callouts > Browse: the built-in gaming templates (Graphics' "builtin" library: template.list's, category "gaming"), the
// essay and documentary ones (0.4, category "essay": they take the project's brand kit colours and fonts), Lottie stickers (animations imported with "Import Lottie sticker…", kept in the project's library as
// "lottie.<name>"), the project's other templates and the user's, searched by name, description or field, with live
// thumbnails. A card is dragged onto the timeline, or added at the playhead with a double-click or Enter (its first
// text layer selected, ready to type). "Save as template…" keeps the selected graphic with the fields chosen.
Item {
    id: root
    objectName: "graphicsBrowsePage"

    property string query: ""
    /// 0.7 (card #87): how the cards are ordered ("name", "nameDesc", "length", "lengthDesc", "fields").
    property string sortKey: "name"
    signal saveRequested()

    /// P1 round (#30, stream 1E): Edit > Find (Ctrl+F) - the search, its text selected.
    function focusFind() {
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    /// The built-in templates' names in the UI language (the catalog's are English).
    readonly property var builtinNames: ({
        "wabcut.gaming.subscribe": qsTr("Subscribe"),
        "wabcut.gaming.killStreak": qsTr("Kill streak"),
        "wabcut.gaming.lowerThird": qsTr("Lower third"),
        "wabcut.gaming.replay": qsTr("Replay"),
        "wabcut.gaming.countdown": qsTr("Countdown"),
        "wabcut.gaming.chatBubble": qsTr("Chat bubble"),
        "wabcut.gaming.emotePop": qsTr("Emote pop"),
        "wabcut.gaming.arrowCallout": qsTr("Arrow callout"),
        "wabcut.gaming.circleCallout": qsTr("Circle callout"),
        "wabcut.gaming.webcamFrame": qsTr("Webcam frame"),
        "wabcut.gaming.endCard": qsTr("End card"),
        "wabcut.gaming.intro": qsTr("Intro"),
        "wabcut.essay.nameTitle": qsTr("Name and title"),
        "wabcut.essay.location": qsTr("Location and date"),
        "wabcut.essay.quote": qsTr("Quote"),
        "wabcut.essay.source": qsTr("Source"),
        "wabcut.essay.chapterCard": qsTr("Chapter card")
    })

    function matches(t) {
        const q = root.query.trim().toLowerCase()
        if (q === "")
            return true
        if ((t.name ?? "").toLowerCase().includes(q) || (t.description ?? "").toLowerCase().includes(q))
            return true
        return (t.fields ?? []).some(f => (f.label ?? "").toLowerCase().includes(q))
    }
    /// 0.7 (card #87): `list` in the chosen order (names break ties).
    function sortList(list) {
        const copy = list.slice()
        const byName = (a, b) => String(a.name ?? "").localeCompare(String(b.name ?? ""))
        switch (root.sortKey) {
        case "nameDesc":
            copy.sort((a, b) => byName(b, a))
            break
        case "length":
            copy.sort((a, b) => ((a.seconds ?? 0) - (b.seconds ?? 0)) || byName(a, b))
            break
        case "lengthDesc":
            copy.sort((a, b) => ((b.seconds ?? 0) - (a.seconds ?? 0)) || byName(a, b))
            break
        case "fields":
            copy.sort((a, b) => ((b.fields ?? []).length - (a.fields ?? []).length) || byName(a, b))
            break
        default:
            copy.sort(byName)
        }
        return copy
    }
    readonly property var sortNames: ({
        "name": qsTr("Name"),
        "nameDesc": qsTr("Name, Z to A"),
        "length": qsTr("Shortest"),
        "lengthDesc": qsTr("Longest"),
        "fields": qsTr("Most fields")
    })
    /// 0.7 (card #87): `list` split by the folder its templates are filed in: [{ name, templates }], by name.
    function foldersOf(list) {
        const byFolder = {}
        for (const t of list) {
            const name = String(t.folder ?? "")
            if (name === "")
                continue
            if (byFolder[name] === undefined)
                byFolder[name] = []
            byFolder[name].push(t)
        }
        return Object.keys(byFolder).sort((a, b) => a.localeCompare(b)).map(name => ({ name: name, templates: byFolder[name] }))
    }
    function isSticker(t) {
        return t.library === "project" && String(t.id ?? "").startsWith("lottie.")
    }
    function openMenu(key) {
        templateMenu.key = key
        templateMenu.popup()
    }
    readonly property var builtinTemplates: root.sortList(Graphics.templates.filter(t => t.library === "builtin")
        .map(t => Object.assign({}, t, { name: root.builtinNames[t.id] ?? t.name })).filter(t => root.matches(t)))
    readonly property var gamingTemplates: root.builtinTemplates.filter(t => t.category === "gaming")
    readonly property var essayTemplates: root.builtinTemplates.filter(t => t.category === "essay")
    readonly property var otherBuiltinTemplates: root.builtinTemplates.filter(t => t.category !== "gaming" && t.category !== "essay")
    readonly property var stickerTemplates: root.sortList(Graphics.templates.filter(t => root.isSticker(t) && root.matches(t)))
    readonly property var projectTemplates: root.sortList(Graphics.templates.filter(t => t.library === "project" && !root.isSticker(t)
                                                                                      && root.matches(t)))
    readonly property var userTemplates: root.sortList(Graphics.templates.filter(t => t.library === "user" && root.matches(t)))

    // A group of cards under its label; `empty` shows when it has none.
    component TemplateGroup: ColumnLayout {
        id: group
        property string title: ""
        property string empty: ""
        property string listName: ""
        property var templates: []
        /// 0.7 (card #87): the group lists the templates filed in folders under their folder's name (a fold you can close).
        property bool foldered: false
        property var collapsed: ({})
        readonly property var loose: group.foldered ? group.templates.filter(t => (t.folder ?? "") === "") : group.templates
        readonly property var folders: group.foldered ? root.foldersOf(group.templates) : []
        signal menuRequested(string key)

        function toggle(name) {
            const next = Object.assign({}, group.collapsed)
            next[name] = next[name] !== true
            group.collapsed = next
        }

        Layout.fillWidth: true
        spacing: Theme.space(2)

        GraphicGroupLabel {
            text: group.title
        }
        Text {
            objectName: group.listName + "Empty"
            Layout.fillWidth: true
            visible: group.templates.length === 0 && group.empty !== ""
            wrapMode: Text.Wrap
            text: group.empty
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Flow {
            objectName: group.listName
            Layout.fillWidth: true
            visible: group.loose.length > 0
            spacing: Theme.space(2)
            Repeater {
                model: group.loose
                delegate: GraphicTemplateCard {
                    width: Math.max(Theme.space(32), (group.width - Theme.space(2)) / 2)
                    onAddRequested: key => Graphics.instantiateAtPlayhead(key)
                    onMenuRequested: (key, at) => group.menuRequested(key)
                }
            }
        }
        // 0.7 (card #87): the folders the templates are filed in (Move to Folder…, Install folder…).
        Repeater {
            model: group.folders
            delegate: ColumnLayout {
                id: fold
                required property var modelData
                readonly property bool open: group.collapsed[fold.modelData.name] !== true
                objectName: group.listName + "Folder_" + fold.modelData.name
                Layout.fillWidth: true
                spacing: Theme.space(1.5)

                PillButton {
                    objectName: group.listName + "FolderToggle_" + fold.modelData.name
                    compact: true
                    variant: "ghost"
                    iconName: fold.open ? "chevron-down" : "chevron-right"
                    text: qsTr("%1 (%2)").arg(fold.modelData.name).arg(fold.modelData.templates.length)
                    tooltip: fold.open ? qsTr("Close this folder") : qsTr("Open this folder")
                    onClicked: group.toggle(fold.modelData.name)
                }
                Flow {
                    objectName: group.listName + "FolderCards_" + fold.modelData.name
                    Layout.fillWidth: true
                    visible: fold.open
                    spacing: Theme.space(2)
                    Repeater {
                        model: fold.modelData.templates
                        delegate: GraphicTemplateCard {
                            width: Math.max(Theme.space(32), (group.width - Theme.space(2)) / 2)
                            onAddRequested: key => Graphics.instantiateAtPlayhead(key)
                            onMenuRequested: (key, at) => group.menuRequested(key)
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            FieldInput {
                id: searchField
                objectName: "graphicsSearch"
                Layout.fillWidth: true
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Search graphics")
                Accessible.name: qsTr("Search graphics")
                onTextChanged: root.query = text
            }
            PillButton {
                id: sortButton
                objectName: "graphicsSort"
                compact: true
                variant: "ghost"
                iconName: "chevron-down"
                text: qsTr("Sort: %1").arg(root.sortNames[root.sortKey] ?? "")
                tooltip: qsTr("How the templates are ordered")
                onClicked: sortMenu.popup(sortButton, 0, sortButton.height + Theme.space(1))
            }
            IconButton {
                objectName: "graphicsRefresh"
                compact: true
                iconName: "rotate-cw"
                tooltip: qsTr("Read the template folders again")
                onClicked: Graphics.refreshTemplates()
            }
        }

        Flickable {
            id: flick
            objectName: "graphicsBrowseFlick"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: column.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: column
                width: flick.width - Theme.space(3)
                spacing: Theme.space(2)

                TemplateGroup {
                    objectName: "calloutsGaming"
                    title: qsTr("Gaming")
                    listName: "graphicsGamingTemplates"
                    templates: root.gamingTemplates
                    empty: root.query !== "" ? qsTr("No callout matches.") : qsTr("Gaming templates arrive with the next update.")
                    onMenuRequested: key => root.openMenu(key)
                }
                // 0.4: essay and documentary graphics (name and title, location, quote, source, chapter card).
                TemplateGroup {
                    objectName: "calloutsEssay"
                    visible: root.essayTemplates.length > 0
                    title: qsTr("Essay & documentary")
                    listName: "graphicsEssayTemplates"
                    templates: root.essayTemplates
                    onMenuRequested: key => root.openMenu(key)
                }
                // Built-in templates of other categories (none yet), when there are some.
                TemplateGroup {
                    objectName: "calloutsMoreBuiltin"
                    visible: root.otherBuiltinTemplates.length > 0
                    title: qsTr("More built-in")
                    listName: "graphicsBuiltinTemplates"
                    templates: root.otherBuiltinTemplates
                    onMenuRequested: key => root.openMenu(key)
                }

                TemplateGroup {
                    objectName: "calloutsStickers"
                    title: qsTr("Stickers")
                    listName: "graphicsStickerTemplates"
                    templates: root.stickerTemplates
                    empty: root.query !== "" ? qsTr("No sticker matches.")
                                             : qsTr("Lottie animations you import as stickers show here, ready to add again.")
                    onMenuRequested: key => root.openMenu(key)
                }
                PillButton {
                    objectName: "calloutsImportSticker"
                    Layout.fillWidth: true
                    compact: true
                    variant: "secondary"
                    iconName: "plus"
                    text: qsTr("Import Lottie sticker…")
                    tooltip: qsTr("Add a Lottie animation (JSON) at the playhead and keep it here as a sticker")
                    onClicked: GraphicsImport.openImport({ saveAsTemplate: true })
                }

                TemplateGroup {
                    objectName: "calloutsProject"
                    title: qsTr("This project")
                    listName: "graphicsProjectTemplates"
                    foldered: true
                    templates: root.projectTemplates
                    empty: root.query !== "" ? qsTr("No project template matches.")
                                             : qsTr("Save a graphic as a template to reuse it in this project.")
                    onMenuRequested: key => root.openMenu(key)
                }
                PillButton {
                    objectName: "graphicsSaveTemplate"
                    Layout.fillWidth: true
                    compact: true
                    variant: "ghost"
                    iconName: "save"
                    text: qsTr("Save as template…")
                    enabled: Graphics.clip !== ""
                    tooltip: Graphics.clip !== "" ? qsTr("Keep the selected graphic as a template, with the fields you choose")
                                                  : qsTr("Select a graphic clip on the timeline first")
                    onClicked: root.saveRequested()
                }
                // P1 round (#50): bring a .mogrt / .wabcut-template.json in as a project template.
                PillButton {
                    objectName: "graphicsInstallTemplate"
                    Layout.fillWidth: true
                    compact: true
                    variant: "ghost"
                    iconName: "download"
                    text: qsTr("Install template…")
                    tooltip: qsTr("Add a Motion Graphics template (.mogrt) or a Wabcut template file to this project")
                    onClicked: Graphics.openInstallDialog("project")
                }
                // 0.7 (card #87): a whole folder of templates, filed under folders named after where they were.
                PillButton {
                    objectName: "graphicsInstallFolder"
                    Layout.fillWidth: true
                    compact: true
                    variant: "ghost"
                    iconName: "folder-plus"
                    text: qsTr("Install folder…")
                    tooltip: qsTr("Add every Motion Graphics template (.mogrt) and Wabcut template file in a folder and its subfolders to this project, filed in folders named after theirs")
                    onClicked: Graphics.openInstallFolderDialog("project")
                }

                TemplateGroup {
                    objectName: "calloutsUser"
                    title: qsTr("My templates")
                    listName: "graphicsUserTemplates"
                    foldered: true
                    templates: root.userTemplates
                    empty: root.query !== "" ? qsTr("None of your templates matches.")
                                             : qsTr("Templates you save to your library show here in every project.")
                    onMenuRequested: key => root.openMenu(key)
                }
                PillButton {
                    objectName: "graphicsInstallFolderUser"
                    Layout.fillWidth: true
                    compact: true
                    variant: "ghost"
                    iconName: "folder-plus"
                    text: qsTr("Install folder into My templates…")
                    tooltip: qsTr("Add a folder of template files to your library, for every project")
                    onClicked: Graphics.openInstallFolderDialog("user")
                }
                Repeater {
                    model: Graphics.libraryErrors
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Not read: %1 (%2)").arg(String(modelData.file ?? "").split(/[\\/]/).pop()).arg(modelData.message ?? "")
                        color: Theme.warning
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
            }
        }
    }

    // A template's menu: add it, or delete it from its library (not a built-in one).
    GraphicTemplateMenu {
        id: templateMenu
        // 0.7 (card #87): Move to Folder...: the folder's name, "" for none.
        onMoveRequested: key => {
            const t = Graphics.templates.find(x => x.key === key)
            folderNames.ask(qsTr("Move to Folder"), t ? (t.folder ?? "") : "", qsTr("Move"), text => Graphics.setTemplateFolder(key, text))
        }
    }
    GraphicSortMenu {
        id: sortMenu
        current: root.sortKey
        onChosen: key => root.sortKey = key
    }
    EffectNameDialog {
        id: folderNames
        objectName: "templateFolderDialog"
    }
}
