pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Command palette (Ctrl/⌘+Shift+P): a fuzzy search over every keymap command (with its group and current shortcut),
// the typed edit commands of the command catalog (with simple parameter prompts, a dry-run preview and apply), and a
// separate "Ask AI…" row. Rows come from CommandPalette (C++); disabled rows stay listed, dimmed, with the reason.
// Enter runs, ↑/↓ choose, Esc goes back or closes. Pops in with a spring (Theme.easeSpring, durBounce; none under
// reduce motion). The AI row opens a placeholder until the AI platform plugs its provider in.
Popup {
    id: root

    objectName: "commandPalette"

    property string mode: "list"     // "list" | "form" | "ai"
    property var current: null       // the row being prompted (form)
    property var values: ({})        // prompt values (form)
    property var check: ({})         // dry run of the form: { ok, summary, error }
    property string message: ""      // why the last run failed
    property string aiQuery: ""
    // Ranked rows in the list; pinned rows ("Ask AI…") sit in a fixed, separated row under it. `selected` runs over
    // both: 0 .. rows.length - 1 in the list, then the pinned rows. 0.1.3: Settings > AI's "AI features" off hides them.
    readonly property var rows: CommandPalette.results.filter(r => !r.pinned)
    readonly property var pinned: Preferences.aiEnabled ? CommandPalette.results.filter(r => r.pinned) : []
    property int selected: 0
    readonly property var selectedRow: selected < rows.length ? rows[selected] : pinned[selected - rows.length]

    function openPalette() {
        mode = "list"
        message = ""
        field.text = ""
        CommandPalette.query = ""
        CommandPalette.refresh()
        open()
    }

    function choose(index) {
        selected = Math.max(0, Math.min(rows.length + pinned.length - 1, index))
        if (selected < rows.length) {
            list.currentIndex = selected
            list.positionViewAtIndex(selected, ListView.Contain)
        } else {
            list.currentIndex = -1
        }
    }

    function runCurrent() {
        const row = selectedRow
        if (!row)
            return
        if (!row.enabled) {
            message = row.reason
            return
        }
        if (row.params && row.params.length > 0) {
            current = row
            values = CommandPalette.defaults(row.key)
            mode = "form"
            refreshCheck()
            return
        }
        if (row.section === "ai") {
            CommandPalette.activate(row.key)   // emits aiRequested
            return
        }
        close()
        const error = CommandPalette.activate(row.key)
        if (error !== "") {
            message = error
            open()
        }
    }

    function setValue(key, value) {
        const next = Object.assign({}, values)
        next[key] = value
        values = next
        refreshCheck()
    }

    function refreshCheck() {
        check = current ? CommandPalette.preview(current.key, values) : ({})
    }

    function applyForm() {
        const error = CommandPalette.activate(current.key, values)
        if (error === "")
            close()
        else
            message = error
    }

    parent: Overlay.overlay
    modal: true
    dim: true
    focus: true
    width: parent ? Math.min(parent.width - Theme.space(16), Theme.space(150)) : Theme.space(150)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round(parent.height * 0.12) : 0
    padding: Theme.space(2)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    transformOrigin: Item.Top

    Overlay.modal: Rectangle {
        color: Theme.scrim
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    background: Rectangle {
        radius: Theme.radiusField + Theme.space(1)
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durBounce; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSpring }
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }

    onOpened: {
        if (mode === "list") {
            field.forceActiveFocus()
            choose(0)
        }
    }
    onRowsChanged: if (mode === "list") choose(0)
    onClosed: current = null

    Connections {
        target: CommandPalette
        function onAiRequested(query) {
            // The editing assistant takes the request (panels/ai/AssistantPanel.qml, AgentController); the palette
            // records the hand-off in `mode` and closes.
            root.aiQuery = query
            root.mode = "ai"
            root.close()
            AgentController.open(query)
        }
    }

    contentItem: Column {
        spacing: Theme.space(2)
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Command palette")

        // ---- Search ----
        TextField {
            id: field
            objectName: "paletteField"
            visible: root.mode === "list"
            width: parent.width
            leftPadding: Theme.space(3)
            rightPadding: Theme.space(3)
            topPadding: Theme.space(2)
            bottomPadding: Theme.space(2)
            placeholderText: qsTr("Type a command, or ask AI…")
            placeholderTextColor: Theme.textTertiary
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            font.family: Theme.fontSans
            font.pixelSize: Theme.textLg
            Accessible.name: qsTr("Search commands")
            background: Rectangle {
                radius: height / 2
                color: Theme.raised
                border.width: field.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.focus
            }
            onTextEdited: {
                root.message = ""
                CommandPalette.query = text
            }
            Keys.onUpPressed: root.choose(root.selected - 1)
            Keys.onDownPressed: root.choose(root.selected + 1)
            Keys.onReturnPressed: root.runCurrent()
            Keys.onEnterPressed: root.runCurrent()
        }

        ListView {
            id: list
            objectName: "paletteList"
            visible: root.mode === "list"
            width: parent.width
            height: Math.min(contentHeight, Theme.space(100))
            clip: true
            model: root.rows
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Commands")

            delegate: Item {
                id: rowItem

                required property var modelData
                required property int index
                readonly property var previous: index > 0 ? root.rows[index - 1] : null
                // Ranked results mix kinds (each row names its group); only recent commands get a header.
                readonly property bool startsSection: modelData.section === "recent" && (!previous || previous.section !== "recent")
                readonly property bool current: ListView.isCurrentItem

                width: ListView.view.width
                height: (startsSection ? sectionHeader.height : 0) + rowBody.height

                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.enabled ? modelData.title
                                                   : qsTr("%1, unavailable: %2").arg(modelData.title).arg(modelData.reason)

                // Section header; the AI row sits apart behind a divider.
                Item {
                    id: sectionHeader
                    visible: rowItem.startsSection
                    width: parent.width
                    height: visible ? Theme.space(7) : 0

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space(2)
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Theme.space(1)
                        text: qsTr("Recent")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                    }
                }

                Rectangle {
                    id: rowBody
                    y: sectionHeader.height
                    width: parent.width
                    height: Theme.controlHeight + (rowItem.modelData.detail !== "" || !rowItem.modelData.enabled ? Theme.space(3) : 0)
                    radius: Theme.radiusField
                    color: rowItem.current ? Theme.sunken : "transparent"
                    border.width: rowItem.current ? Theme.hairline : 0
                    border.color: Theme.accentStroke

                    Icon {
                        id: rowIcon
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        size: Theme.iconSm
                        name: rowItem.modelData.section === "edits" ? "pencil" : "chevron-down"
                        rotation: rowItem.modelData.section === "edits" ? 0 : -90
                        color: Theme.textTertiary
                    }

                    Column {
                        anchors.left: rowIcon.right
                        anchors.leftMargin: Theme.space(2)
                        anchors.right: shortcutChip.left
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: rowItem.modelData.enabled ? 1 : Theme.disabledOpacity

                        Row {
                            spacing: Theme.space(2)
                            Text {
                                text: rowItem.modelData.title
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: rowItem.modelData.group
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                        Text {
                            visible: text !== ""
                            width: parent.width
                            text: rowItem.modelData.enabled ? rowItem.modelData.detail : rowItem.modelData.reason
                            elide: Text.ElideRight
                            color: rowItem.modelData.enabled ? Theme.textTertiary : Theme.warning
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }

                    Rectangle {
                        id: shortcutChip
                        visible: rowItem.modelData.shortcut !== ""
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        width: visible ? chipText.implicitWidth + Theme.space(3) : 0
                        height: chipText.implicitHeight + Theme.space(1)
                        radius: Theme.radiusBadge
                        color: Theme.sunken
                        border.width: Theme.hairline
                        border.color: Theme.border

                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text: rowItem.modelData.shortcut
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }

                    TapHandler {
                        onTapped: {
                            root.choose(rowItem.index)
                            root.runCurrent()
                        }
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }

        // Nothing matches: say so (the AI row below can still take the request).
        EmptyState {
            objectName: "paletteNoMatch"
            width: parent.width
            topPadding: Theme.space(2)
            bottomPadding: Theme.space(2)
            visible: root.mode === "list" && root.rows.length === 0 && CommandPalette.query.trim() !== ""
            text: root.pinned.length > 0 ? qsTr("No command matches “%1”. Ask AI below.").arg(CommandPalette.query.trim())
                                         : qsTr("No command matches “%1”.").arg(CommandPalette.query.trim())
        }

        // ---- Pinned rows: Ask AI…, apart from the commands ----
        Column {
            visible: root.mode === "list" && root.pinned.length > 0
            width: parent.width
            spacing: Theme.space(1)

            Rectangle {
                width: parent.width
                height: Theme.hairline
                color: Theme.border
            }

            Repeater {
                model: root.pinned

                delegate: Rectangle {
                    id: pinnedRow

                    required property var modelData
                    required property int index
                    readonly property bool current: root.selected === root.rows.length + index

                    objectName: "paletteAskAi"
                    width: parent ? parent.width : 0
                    height: Theme.controlHeight + Theme.space(3)
                    radius: Theme.radiusField
                    color: current ? Theme.accentWash : "transparent"
                    border.width: current ? Theme.hairline : 0
                    border.color: Theme.accentStroke
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.title

                    Icon {
                        id: aiIcon
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        size: Theme.iconSm
                        name: "sparkles"
                        color: Theme.accentStroke
                    }
                    Column {
                        anchors.left: aiIcon.right
                        anchors.leftMargin: Theme.space(2)
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            text: pinnedRow.modelData.title
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            text: pinnedRow.modelData.detail
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    TapHandler {
                        onTapped: {
                            root.choose(root.rows.length + pinnedRow.index)
                            root.runCurrent()
                        }
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }

        // ---- Parameters of an edit command ----
        Column {
            id: form
            objectName: "paletteForm"
            visible: root.mode === "form"
            width: parent.width
            spacing: Theme.space(2)

            Text {
                text: root.current ? root.current.title : ""
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: root.current ? root.current.detail : ""
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            Repeater {
                model: root.current ? root.current.params : []

                delegate: Row {
                    id: paramRow

                    required property var modelData
                    readonly property var value: root.values[modelData.key]

                    width: form.width
                    spacing: Theme.space(3)

                    Text {
                        width: Theme.space(28)
                        anchors.verticalCenter: parent.verticalCenter
                        text: paramRow.modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }

                    Loader {
                        anchors.verticalCenter: parent.verticalCenter
                        width: form.width - Theme.space(31)
                        sourceComponent: paramRow.modelData.kind === "color" ? colorEditor
                                       : paramRow.modelData.kind === "boolean" ? boolEditor
                                       : (paramRow.modelData.kind === "choice" || paramRow.modelData.kind === "track") ? choiceEditor
                                       : textEditor

                        Component {
                            id: textEditor
                            TextField {
                                text: paramRow.value !== undefined ? String(paramRow.value) : ""
                                id: paramField
                                padding: Theme.space(2)
                                leftPadding: Theme.space(3.5)
                                rightPadding: Theme.space(3.5)
                                color: Theme.textPrimary
                                font.family: paramRow.modelData.kind === "timecode" ? Theme.fontMono : Theme.fontSans
                                font.pixelSize: Theme.textMd
                                inputMethodHints: paramRow.modelData.kind === "integer" || paramRow.modelData.kind === "number"
                                                  ? Qt.ImhFormattedNumbersOnly : Qt.ImhNone
                                Accessible.name: paramRow.modelData.label
                                background: Rectangle {
                                    radius: height / 2
                                    color: Theme.raised
                                    border.width: paramField.activeFocus ? Theme.focusRingWidth : 0
                                    border.color: Theme.focus
                                }
                                onTextEdited: {
                                    const kind = paramRow.modelData.kind
                                    root.setValue(paramRow.modelData.key, kind === "integer" ? parseInt(text) || 0
                                                                        : kind === "number" ? Number(text) || 0 : text)
                                }
                                Keys.onReturnPressed: root.applyForm()
                                Keys.onEnterPressed: root.applyForm()
                            }
                        }
                        Component {
                            id: boolEditor
                            Toggle {
                                checked: paramRow.value === true
                                Accessible.name: paramRow.modelData.label
                                onToggled: root.setValue(paramRow.modelData.key, checked)
                            }
                        }
                        Component {
                            id: choiceEditor
                            Flow {
                                spacing: Theme.space(1)
                                Repeater {
                                    model: paramRow.modelData.options
                                    delegate: PillButton {
                                        required property string modelData
                                        text: modelData
                                        checkable: true
                                        checked: paramRow.value === modelData
                                        variant: checked ? "primary" : "secondary"
                                        onClicked: root.setValue(paramRow.modelData.key, modelData)
                                    }
                                }
                            }
                        }
                        Component {
                            id: colorEditor
                            Row {
                                spacing: Theme.space(0.5)
                                Repeater {
                                    model: Theme.clipColors.length
                                    delegate: Swatch {
                                        required property int index
                                        swatchColor: Theme.clipColor(index)
                                        checked: paramRow.value === index
                                        tooltip: qsTr("Label color %1").arg(index + 1)
                                        onClicked: root.setValue(paramRow.modelData.key, index)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Dry run: what applying would change (nothing changes yet).
            Text {
                objectName: "palettePreview"
                width: parent.width
                text: root.check.ok ? qsTr("Preview: %1").arg(root.check.summary) : (root.check.error ?? "")
                color: root.check.ok ? Theme.textSecondary : Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.WordWrap
            }

            Row {
                spacing: Theme.space(2)
                PillButton {
                    text: qsTr("Apply")
                    variant: "primary"
                    enabled: root.check.ok === true
                    onClicked: root.applyForm()
                }
                PillButton {
                    text: qsTr("Back")
                    variant: "secondary"
                    onClicked: {
                        root.mode = "list"
                        field.forceActiveFocus()
                    }
                }
            }
        }

        // ---- Ask AI (placeholder until the AI platform plugs in) ----
        Column {
            objectName: "paletteAiPanel"
            visible: root.mode === "ai"
            width: parent.width
            spacing: Theme.space(3)

            Row {
                spacing: Theme.space(2)
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    size: Theme.iconMd
                    name: "sparkles"
                    color: Theme.accentStroke
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("AI assistant is not configured")
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textTitle
                    font.weight: Font.DemiBold
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Describing an edit in your own words will work once an AI provider is set up. Your request is kept here:")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Rectangle {
                width: parent.width
                height: requestText.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Text {
                    id: requestText
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    text: root.aiQuery !== "" ? root.aiQuery : qsTr("(no request typed)")
                    wrapMode: Text.WordWrap
                    color: root.aiQuery !== "" ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
            }
            PillButton {
                id: backButton
                text: qsTr("Back to commands")
                variant: "secondary"
                onClicked: {
                    root.mode = "list"
                    field.forceActiveFocus()
                }
            }
        }

        // ---- Hints and errors ----
        Text {
            width: parent.width
            text: root.message !== "" ? root.message
                                      : qsTr("↑ ↓ choose · Enter run · Esc close")
            color: root.message !== "" ? Theme.warning : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            horizontalAlignment: Text.AlignRight
        }
    }
}
