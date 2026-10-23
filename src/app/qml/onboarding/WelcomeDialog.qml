pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// The first-run welcome, "Coming from Premiere?" (M5j onboarding; fun, skippable, never in the way). Five short pages:
//   0 hello · 1 your keys (Premiere's defaults, or import a .kys) · 2 where you start and your look (workspace, accent,
//   dark or light) · 3 AI on your terms (M5n: each AI feature and where it runs, on this computer or with your own
//   provider key; consent and the no-cloud lock) · 4 ready (the three-step tour, the Keyboard Trainer, or just start
//   editing).
// Every choice applies at once, behind the scrim. Skip (or Esc) on any page ends it; either way it is not shown again
// (Preferences.welcomeDone) and Help runs it again. `finished` says how it ended: "skipped", "done", "tour" or
// "trainer".
Popup {
    id: dialog
    objectName: "welcomeDialog"

    property int page: 0
    readonly property int pageCount: 5
    // The AI page: where each feature runs (docs/ai.md). "local": on this computer; "key": your provider's key (asks
    // first); "either": on this computer by default, or with your key.
    readonly property var aiFeatures: [
        { key: "transcript", icon: "captions", where: "either", title: qsTr("Transcript editing"),
          text: qsTr("Transcribed on this computer, or with your speech-to-text key") },
        { key: "assistant", icon: "sparkles", where: "either", title: qsTr("The editing assistant"),
          text: qsTr("A local model, or your Claude, OpenAI or Gemini key; every edit previews and undoes in one step") },
        { key: "enhance", icon: "audio-lines", where: "local", title: qsTr("Enhance Speech and stems"),
          text: qsTr("Cleaned up and separated on this computer") },
        { key: "captions", icon: "type", where: "either", title: qsTr("Translate and dub captions"),
          text: qsTr("Translation runs on this computer; a dub uses your voice provider's key") },
        { key: "search", icon: "search", where: "local", title: qsTr("Smart search"),
          text: qsTr("Your footage is indexed on this computer") },
        { key: "multicam", icon: "layers", where: "local", title: qsTr("Multicam auto-switch"),
          text: qsTr("Who is speaking is worked out on this computer") }
    ]
    function whereLabel(where) {
        return where === "local" ? qsTr("On this computer") : where === "key" ? qsTr("Your key") : qsTr("Local or your key")
    }
    property int workspace: 0
    property string keysChoice: "premiere"
    property string importMessage: ""
    property bool importFailed: false
    property string outcome: ""
    // The accents' names (design language v3: Violet, Pink, Lime, Orange, Blue).
    readonly property var accentLabels: Theme.accentLabels

    signal workspaceChosen(int workspace)
    signal finished(string outcome)

    /// Opens on the first page; `currentWorkspace` is Main's.
    function begin(currentWorkspace) {
        dialog.page = 0
        dialog.workspace = currentWorkspace === 2 ? 0 : currentWorkspace
        dialog.outcome = ""
        dialog.importMessage = ""
        dialog.importFailed = false
        dialog.keysChoice = Shortcuts.preset === "premiere" && !Shortcuts.modified ? "premiere" : "custom"
        dialog.open()
    }
    function finish(how) {
        dialog.outcome = how
        Preferences.welcomeDone = true
        dialog.close()
    }
    function goTo(index) {
        dialog.page = Math.max(0, Math.min(dialog.pageCount - 1, index))
        Qt.callLater(() => nextButton.visible ? nextButton.forceActiveFocus(Qt.TabFocusReason)
                                              : tourButton.forceActiveFocus(Qt.TabFocusReason))
    }
    function usePremiereKeys() {
        dialog.keysChoice = "premiere"
        dialog.importMessage = ""
        dialog.importFailed = false
        if (Shortcuts.preset !== "premiere")
            Shortcuts.applyPreset("premiere")
    }
    /// Imports a Premiere key set (merged over the current keys, as Premiere does).
    function importKeySet(url) {
        const report = Shortcuts.importKeySet(url)
        dialog.importFailed = !report.ok
        if (!report.ok) {
            dialog.importMessage = qsTr("Cannot import %1: %2").arg(report.fileName).arg(report.error)
            return false
        }
        dialog.keysChoice = "kys"
        //: %1 = file name, %2 = number of commands with new keys
        dialog.importMessage = qsTr("Imported %1: %n command(s) got your keys.", "", report.changed).arg(report.fileName)
        if ((report.unmapped ?? []).length > 0)
            dialog.importMessage += " " + qsTr("%n Premiere command(s) Wabcut does not have yet were skipped.", "",
                                                report.unmapped.length)
        return true
    }
    function chooseWorkspace(id) {
        dialog.workspace = id
        Preferences.startWorkspace = id
        dialog.workspaceChosen(id)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(180), Theme.space(180))
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Theme.pad + Theme.space(3)

    onAboutToShow: Qt.callLater(() => nextButton.forceActiveFocus(Qt.TabFocusReason))
    onClosed: {
        if (dialog.outcome === "") {
            dialog.outcome = "skipped"
            Preferences.welcomeDone = true
        }
        dialog.finished(dialog.outcome)
    }

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }
    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durState; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }

    FileDialog {
        id: kysDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Premiere Pro key sets (*.kys)")]
        onAccepted: dialog.importKeySet(selectedFile)
    }

    // A large radio choice: icon, title, one line. Raised; the chosen one is filled with the accent, black on it
    // (design language v3: Home's format tiles).
    component ChoiceCard: AbstractButton {
        id: card
        property string iconName: ""
        property string detail: ""
        property bool selected: false

        Layout.fillWidth: true
        implicitHeight: Math.max(Theme.space(16), cardRow.implicitHeight + topPadding + bottomPadding)
        padding: Theme.space(3)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: card.text
        Accessible.description: card.detail
        Accessible.checked: card.selected
        Keys.onReturnPressed: card.clicked()
        Keys.onEnterPressed: card.clicked()

        background: Rectangle {
            radius: Theme.radiusCard
            color: card.selected ? (card.hovered ? Qt.lighter(Theme.accentFill, 1.06) : Theme.accentFill)
                 : card.hovered ? Theme.hover : Theme.raised
            border.width: card.visualFocus ? Theme.focusRingWidth : 0
            border.color: Theme.focus
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        contentItem: RowLayout {
            id: cardRow
            spacing: Theme.space(3)
            Icon {
                Layout.alignment: Qt.AlignTop
                name: card.iconName
                size: Theme.iconMd
                color: card.selected ? Theme.accentOnFill : Theme.textSecondary
                visible: card.iconName !== ""
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(0.5)
                Text {
                    Layout.fillWidth: true
                    text: card.text
                    color: card.selected ? Theme.accentOnFill : Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    font.weight: card.selected ? Theme.weightTitle : Theme.weightLabel
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.detail !== ""
                    text: card.detail
                    color: card.selected ? Theme.accentOnFill : Theme.textSecondary
                    opacity: card.selected ? 0.78 : 1
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: card.selected ? Font.Medium : Font.Normal
                    wrapMode: Text.Wrap
                }
            }
            Icon {
                Layout.alignment: Qt.AlignTop
                name: "check"
                size: Theme.iconSm
                color: Theme.accentOnFill
                visible: card.selected
            }
        }
        HoverHandler {
            enabled: card.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }

    component PageTitle: RowLayout {
        id: pageTitle
        property string text: ""
        Layout.fillWidth: true
        spacing: Theme.space(2)
        SliceMark {
            Layout.alignment: Qt.AlignVCenter
            implicitHeight: Theme.textDisplay
        }
        Text {
            Layout.fillWidth: true
            text: pageTitle.text
            color: Theme.textPrimary
            font.family: Theme.fontDisplay
            font.pixelSize: Theme.textDisplay
            font.weight: Theme.weightTitle
            font.letterSpacing: -0.02 * Theme.textDisplay
            wrapMode: Text.Wrap
            Accessible.role: Accessible.Heading
            Accessible.name: pageTitle.text
        }
    }

    component Body: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(5)
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Welcome to Wabcut")

        // Header: the logo, then Skip.
        RowLayout {
            Layout.fillWidth: true
            BrandLogo {
                showVersion: false
                markSize: Theme.space(8)
            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "welcomeSkipButton"
                variant: "ghost"
                compact: true
                text: dialog.page === dialog.pageCount - 1 ? qsTr("Close") : qsTr("Skip")
                tooltip: qsTr("Skip the welcome (Esc). Run it again from the Help menu.")
                onClicked: dialog.finish(dialog.page === dialog.pageCount - 1 ? "done" : "skipped")
            }
        }

        StackLayout {
            id: pages
            objectName: "welcomePages"
            Layout.fillWidth: true
            currentIndex: dialog.page

            // ---- 0: hello ----
            ColumnLayout {
                spacing: Theme.space(4)
                PageTitle {
                    objectName: "welcomeHero"
                    text: qsTr("Coming from Premiere?")
                }
                Body {
                    text: qsTr("Familiar shortcuts and editing tools. Pick your keys and your look, then take a twenty-second tour.")
                }
                // A run of Premiere's best-known keys lighting up in turn.
                Row {
                    id: keyRun
                    property int lit: -1
                    readonly property var keys: ["J", "K", "L", "I", "O", ",", ".", "Q", "W", "C"]
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Theme.space(2)
                    spacing: Theme.space(2)
                    Repeater {
                        model: keyRun.keys
                        delegate: Keycap {
                            required property string modelData
                            required property int index
                            text: modelData
                            large: true
                            lit: keyRun.lit === index
                        }
                    }
                    Timer {
                        running: dialog.visible && dialog.page === 0 && !Theme.reduceMotion
                        repeat: true
                        interval: 260
                        onTriggered: keyRun.lit = (keyRun.lit + 1) % (keyRun.keys.length + 3)
                    }
                }
                // What carries over.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space(3)
                    spacing: Theme.space(2)
                    Repeater {
                        model: [
                            { icon: "keyboard", title: qsTr("Premiere's keys"), text: qsTr("J, K, L, Ctrl+K, Q and W, or your own .kys") },
                            { icon: "scissors", title: qsTr("Premiere's edits"), text: qsTr("Insert, Overwrite, Ripple, Roll, Slip and Slide") },
                            { icon: "droplet", title: qsTr("Your own look"), text: qsTr("Five accents, dark or light, high contrast") }
                        ]
                        delegate: Rectangle {
                            id: promise
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: promiseColumn.implicitHeight + Theme.space(8)
                            radius: Theme.radiusCard
                            color: Theme.section

                            ColumnLayout {
                                id: promiseColumn
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.space(4)
                                spacing: Theme.space(1)
                                Icon {
                                    name: promise.modelData.icon
                                    size: Theme.iconMd
                                    color: Theme.accentStroke
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: promise.modelData.title
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    font.weight: Theme.weightLabel
                                    wrapMode: Text.Wrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: promise.modelData.text
                                    color: Theme.textSecondary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                }
            }

            // ---- 1: keys ----
            ColumnLayout {
                spacing: Theme.space(3)
                PageTitle {
                    text: qsTr("Your keys")
                }
                Body {
                    text: qsTr("Wabcut starts with Premiere Pro's shortcuts. Bring your own set if you changed them.")
                }
                ChoiceCard {
                    objectName: "welcomeKeysPremiere"
                    iconName: "keyboard"
                    text: qsTr("Premiere Pro defaults")
                    detail: qsTr("The shortcuts Premiere ships with, for Windows and macOS.")
                    selected: dialog.keysChoice === "premiere"
                    onClicked: dialog.usePremiereKeys()
                }
                ChoiceCard {
                    objectName: "welcomeKeysImport"
                    iconName: "import"
                    text: qsTr("Import my key set (.kys)…")
                    detail: qsTr("In Premiere: Edit > Keyboard Shortcuts, then save your set. Wabcut reads the .kys file.")
                    selected: dialog.keysChoice === "kys"
                    onClicked: kysDialog.open()
                }
                Text {
                    objectName: "welcomeImportMessage"
                    Layout.fillWidth: true
                    visible: dialog.importMessage !== ""
                    text: dialog.importMessage
                    color: dialog.importFailed ? Theme.danger : Theme.success
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.Wrap
                }
                // A taste of the active keys (they follow an import at once).
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(3)
                    Repeater {
                        model: ["edit.rippleDelete", "sequence.addEdit", "tool.razor", "marker.markIn"]
                        delegate: Row {
                            id: sample
                            required property string modelData
                            readonly property string keys: KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(modelData) : ""
                            spacing: Theme.space(1.5)
                            Keycap {
                                anchors.verticalCenter: parent.verticalCenter
                                text: sample.keys !== "" ? sample.keys : "—"
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: Shortcuts.revision >= 0 ? Shortcuts.commandName(sample.modelData) : ""
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                }
            }

            // ---- 2: workspace and look ----
            ColumnLayout {
                spacing: Theme.space(3)
                PageTitle {
                    text: qsTr("Where you start, and your look")
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.space(2)
                    rowSpacing: Theme.space(2)
                    Repeater {
                        model: [
                            { id: 0, key: "edit", icon: "scissors", label: qsTr("Edit"), detail: qsTr("Cut and trim; Premiere's Editing layout") },
                            { id: 3, key: "color", icon: "droplet", label: qsTr("Color"), detail: qsTr("Scopes beside the Color panel") },
                            { id: 1, key: "audio", icon: "audio-lines", label: qsTr("Audio"), detail: qsTr("The mixer and the audio tools") },
                            { id: 4, key: "captions", icon: "captions", label: qsTr("Captions"), detail: qsTr("Transcript-first editing and captions") }
                        ]
                        delegate: ChoiceCard {
                            required property var modelData
                            objectName: "welcomeWorkspace_" + modelData.key
                            iconName: modelData.icon
                            text: modelData.label
                            detail: modelData.detail
                            selected: dialog.workspace === modelData.id
                            onClicked: dialog.chooseWorkspace(modelData.id)
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space(1)
                    spacing: Theme.space(4)
                    Text {
                        text: qsTr("Accent")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Medium
                    }
                    Row {
                        spacing: Theme.space(2)
                        Repeater {
                            model: Theme.accentOrder
                            delegate: Swatch {
                                required property string modelData
                                objectName: "welcomeAccent_" + modelData
                                swatchColor: Theme.accents[modelData].fill
                                tooltip: qsTr("Accent: %1").arg(dialog.accentLabels[modelData])
                                checked: Theme.canonicalAccent(Theme.accentName) === modelData
                                onClicked: {
                                    checked = Qt.binding(() => Theme.canonicalAccent(Theme.accentName) === modelData)
                                    Theme.accentName = modelData
                                }
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    PillTabs {
                        objectName: "welcomeThemeTabs"
                        Layout.preferredWidth: Theme.space(48)
                        compact: true
                        model: [{ label: qsTr("Dark"), icon: "moon" }, { label: qsTr("Light"), icon: "sun" }]
                        currentIndex: Theme.dark ? 0 : 1
                        onActivated: index => Theme.dark = index === 0
                        Accessible.name: qsTr("Theme")
                    }
                }
            }

            // ---- 3: AI on your terms ----
            ColumnLayout {
                objectName: "welcomeAiPage"
                spacing: Theme.space(3)
                PageTitle {
                    text: qsTr("AI on your terms")
                }
                Body {
                    text: qsTr("AI in Wabcut edits through the same undoable commands you use. Nothing leaves this computer unless you add your own provider key and allow it.")
                }
                Repeater {
                    model: dialog.aiFeatures
                    delegate: RowLayout {
                        id: feature
                        required property var modelData
                        objectName: "welcomeAi_" + modelData.key
                        Layout.fillWidth: true
                        spacing: Theme.space(3)
                        Accessible.role: Accessible.ListItem
                        Accessible.name: qsTr("%1: %2. %3").arg(modelData.title).arg(dialog.whereLabel(modelData.where)).arg(modelData.text)

                        Icon {
                            Layout.alignment: Qt.AlignVCenter
                            name: feature.modelData.icon
                            size: Theme.iconMd
                            color: Theme.accentStroke
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: feature.modelData.title
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Theme.weightLabel
                                wrapMode: Text.Wrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: feature.modelData.text
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                                wrapMode: Text.Wrap
                            }
                        }
                        StatusPill {
                            Layout.alignment: Qt.AlignVCenter
                            label: dialog.whereLabel(feature.modelData.where)
                            dotColor: feature.modelData.where === "local" ? Theme.success : Theme.accentStroke
                        }
                    }
                }
                Body {
                    objectName: "welcomeAiPrivacy"
                    text: qsTr("A cloud call first asks for your consent, shows what is sent and where, and stays within your monthly spending cap. The no-cloud lock in Settings > AI providers and privacy keeps a project entirely on this computer.")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- 4: ready ----
            ColumnLayout {
                spacing: Theme.space(3)
                PageTitle {
                    text: qsTr("You're set")
                }
                Body {
                    text: qsTr("Three quick stops show where things are: the Library, the Timeline and Export. Or warm up your fingers first.")
                }
                ChoiceCard {
                    id: tourButton
                    objectName: "welcomeTourButton"
                    iconName: "arrow-right"
                    text: qsTr("Take the tour")
                    detail: qsTr("Library, Timeline, Export: three steps")
                    selected: true
                    onClicked: dialog.finish("tour")
                }
                ChoiceCard {
                    objectName: "welcomeTrainerButton"
                    iconName: "zap"
                    text: qsTr("Keyboard Trainer")
                    detail: qsTr("Wabcut names an action; you press its key. How fast is your Ripple Delete?")
                    onClicked: dialog.finish("trainer")
                }
                Body {
                    text: qsTr("Run any of this again from the Help menu, or the command palette.")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.textSm
                }
            }
        }

        // Footer: where you are, Back and Next.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Row {
                objectName: "welcomeProgress"
                spacing: Theme.space(1)
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Step %1 of %2").arg(dialog.page + 1).arg(dialog.pageCount)
                // Page dots; the current page's is a short pill.
                Repeater {
                    model: dialog.pageCount
                    delegate: Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: index === dialog.page ? Theme.space(5) : Theme.dotSize
                        height: Theme.dotSize
                        radius: height / 2
                        color: index === dialog.page ? Theme.accentStroke : Theme.borderStrong
                        Behavior on width {
                            enabled: !Theme.reduceMotion
                            NumberAnimation { duration: Theme.durState; easing.type: Easing.OutCubic }
                        }
                    }
                }

            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "welcomeBackButton"
                visible: dialog.page > 0
                variant: "ghost"
                text: qsTr("Back")
                onClicked: dialog.goTo(dialog.page - 1)
            }
            PillButton {
                id: nextButton
                objectName: "welcomeNextButton"
                visible: dialog.page < dialog.pageCount - 1
                variant: "primary"
                text: dialog.page === 0 ? qsTr("Let's go") : qsTr("Next")
                onClicked: dialog.goTo(dialog.page + 1)
                Keys.onReturnPressed: dialog.goTo(dialog.page + 1)
                Keys.onEnterPressed: dialog.goTo(dialog.page + 1)
            }
            PillButton {
                objectName: "welcomeStartButton"
                visible: dialog.page === dialog.pageCount - 1
                variant: "secondary"
                text: qsTr("Start editing")
                onClicked: dialog.finish("done")
            }
        }
    }
}
