pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Rights and Clearance Log… (0.7): the clearance report documentary makers hand to legal. One row for every source
// the sequence uses (or every file of the project): its clearance status, where it came from, the licence, territory,
// expiry, cost and credit line, how often it is used, and what needs attention on the day the film goes out - material
// that is not cleared, a licence that has expired or runs out within 30 days. Pick rows (click, Ctrl/Shift+click, or the
// arrow keys) to edit their rights together; only the fields you change are written, as one undo step (media.setRights).
// Export log… writes the whole log as a spreadsheet (CSV) or JSON; Make credits roll… puts a rolling credits graphic of the
// credit lines at the end of the sequence (sequence.creditsRoll). Fed by the `Rights` singleton.
WabcutDialog {
    id: dialog
    objectName: "rightsLogDialog"

    /// "sequence" (the sources the open sequence uses) or "project" (every file).
    property string scope: "sequence"
    property bool flaggedOnly: false
    /// The day licences are judged on (YYYY-MM-DD): usually the delivery date.
    property string asOf: Rights.today()
    /// Rights.log(): { ok, error, rows, totals, asOf }.
    property var result: ({ ok: false, error: "", rows: [], totals: ({}) })
    /// The selected rows by media key (true).
    property var selected: ({})
    property string anchorKey: ""
    /// The fields the person changed in the editor (name -> true): only these are written.
    property var edited: ({})
    property string errorText: ""
    property string noticeText: ""

    readonly property var rows: dialog.result.rows ?? []
    readonly property var totals: dialog.result.totals ?? ({})
    readonly property var selectedKeys: {
        const out = []
        for (const row of dialog.rows) {
            if (dialog.selected[row.key] === true)
                out.push(row.key)
        }
        return out
    }
    readonly property int editedCount: Object.keys(dialog.edited).length
    readonly property var statusIds: ["", "cleared", "pending", "fairUse", "notRequired"]
    readonly property var statusNames: [qsTr("Not set"), qsTr("Cleared"), qsTr("Pending"), qsTr("Fair use"), qsTr("Not required")]

    /// The credits roll was made (credit lines, sources without one).
    signal rollMade(int credits, int missing)
    /// The log was written to a file.
    signal written(string path, int rows)

    function openDialog(keys: var) {
        dialog.scope = ProjectController.sequenceNames.length > 0 ? "sequence" : "project"
        dialog.flaggedOnly = false
        dialog.asOf = Rights.today()
        dialog.selected = ({})
        dialog.edited = ({})
        dialog.errorText = ""
        dialog.noticeText = ""
        dialog.refresh()
        if (keys !== undefined && keys !== null && keys.length > 0) {
            const next = ({})
            for (const key of keys) {
                if (dialog.rows.some(r => r.key === key))
                    next[key] = true
            }
            if (Object.keys(next).length === 0 && dialog.scope === "sequence") {
                // The item is not used in the sequence: show the whole project.
                dialog.scope = "project"
                dialog.refresh()
                for (const key of keys) {
                    if (dialog.rows.some(r => r.key === key))
                        next[key] = true
                }
            }
            dialog.selected = next
            dialog.anchorKey = keys[0]
            dialog.loadEditor()
        }
        open()
        rowList.forceActiveFocus()
    }
    function refresh() {
        dialog.result = Rights.log({ scope: dialog.scope, flaggedOnly: dialog.flaggedOnly, asOf: dialog.asOf })
        // Rows that went (a filter, a deleted item) leave the selection.
        const keep = ({})
        for (const row of dialog.rows) {
            if (dialog.selected[row.key] === true)
                keep[row.key] = true
        }
        dialog.selected = keep
        dialog.loadEditor()
    }
    /// The editor shows what the selected rows share; fields that differ stay empty ("various") until typed in.
    property var various: ({})
    function loadEditor() {
        const picked = dialog.rows.filter(r => dialog.selected[r.key] === true)
        const fields = ["source", "licence", "reference", "territory", "expiry", "currency", "creditLine", "notes"]
        const differs = ({})
        const values = ({})
        for (const name of fields) {
            const first = picked.length > 0 ? (picked[0][name] ?? "") : ""
            values[name] = first
            differs[name] = picked.some(r => (r[name] ?? "") !== first)
            if (differs[name])
                values[name] = ""
        }
        const firstCost = picked.length > 0 ? (picked[0].cost ?? "") : ""
        const costDiffers = picked.some(r => (r.cost ?? "") !== firstCost)
        values.cost = costDiffers ? "" : (firstCost ?? "")
        differs.cost = costDiffers
        const firstStatus = picked.length > 0 ? (picked[0].status ?? "") : ""
        const statusDiffers = picked.some(r => (r.status ?? "") !== firstStatus)
        differs.status = statusDiffers
        sourceField.text = values.source
        licenceField.text = values.licence
        referenceField.text = values.reference
        territoryField.text = values.territory
        expiryField.text = values.expiry
        costField.text = values.cost
        currencyField.text = values.currency
        creditField.text = values.creditLine
        notesField.text = values.notes
        statusTabs.currentIndex = statusDiffers ? -1 : Math.max(0, dialog.statusIds.indexOf(firstStatus))
        dialog.various = differs
        dialog.edited = ({})
    }
    function markEdited(name: string) {
        const next = Object.assign({}, dialog.edited)
        next[name] = true
        dialog.edited = next
        dialog.errorText = ""
        dialog.noticeText = ""
    }
    function apply() {
        const fields = ({})
        const sources = {
            source: sourceField.text, licence: licenceField.text, reference: referenceField.text,
            territory: territoryField.text, expiry: expiryField.text, cost: costField.text,
            currency: currencyField.text.toUpperCase(), creditLine: creditField.text, notes: notesField.text
        }
        for (const name of Object.keys(dialog.edited)) {
            if (name === "status")
                fields.status = dialog.statusIds[Math.max(0, statusTabs.currentIndex)]
            else
                fields[name] = sources[name]
        }
        const error = Rights.setRights(dialog.selectedKeys, fields)
        if (error !== "") {
            dialog.errorText = error
            return
        }
        dialog.errorText = ""
        dialog.noticeText = qsTr("Rights saved for %n item(s).", "", dialog.selectedKeys.length)
        dialog.refresh() // the revision signal refreshes too; this keeps the editor in step at once
    }
    function select(key: string, ctrl: bool, shift: bool) {
        const next = ctrl ? Object.assign({}, dialog.selected) : ({})
        if (shift && dialog.anchorKey !== "") {
            const keys = dialog.rows.map(r => r.key)
            const a = keys.indexOf(dialog.anchorKey)
            const b = keys.indexOf(key)
            for (let i = Math.min(a, b); i <= Math.max(a, b) && i >= 0; ++i)
                next[keys[i]] = true
        } else if (ctrl && next[key] === true) {
            delete next[key]
            dialog.anchorKey = key
        } else {
            next[key] = true
            dialog.anchorKey = key
        }
        dialog.selected = next
        dialog.loadEditor()
    }
    function statusColor(status: string): color {
        return status === "cleared" ? Theme.success
             : status === "pending" ? Theme.warning
             : status === "fairUse" ? Theme.info
             : status === "notRequired" ? Theme.textTertiary
             : Theme.textDisabled
    }
    function detailText(row: var): string {
        const parts = []
        if (row.source) parts.push(row.source)
        if (row.licence) parts.push(row.licence)
        if (row.territory) parts.push(row.territory)
        if (row.expiry) parts.push(qsTr("until %1").arg(row.expiry))
        if (row.cost) parts.push(row.cost + (row.currency ? " " + row.currency : ""))
        return parts.join(" · ")
    }
    function summaryText(): string {
        const t = dialog.totals
        if (dialog.result.ok !== true)
            return ""
        const s = t.statuses ?? ({})
        const parts = [qsTr("%n source(s)", "", t.items ?? 0)]
        if ((s["Cleared"] ?? 0) > 0) parts.push(qsTr("%1 cleared").arg(s["Cleared"]))
        if ((s["Pending"] ?? 0) > 0) parts.push(qsTr("%1 pending").arg(s["Pending"]))
        if ((s["Fair use"] ?? 0) > 0) parts.push(qsTr("%1 fair use").arg(s["Fair use"]))
        if ((t.withoutRecord ?? 0) > 0) parts.push(qsTr("%1 without a record").arg(t.withoutRecord))
        for (const cost of (t.costs ?? []))
            parts.push(qsTr("%1 %2 in licences").arg(cost.total).arg(cost.currency !== "" ? cost.currency : qsTr("(no currency)")))
        return parts.join(" · ")
    }

    title: qsTr("Rights and Clearance Log")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(200), Theme.space(220))

    Connections {
        target: Rights
        function onRevisionChanged() {
            if (dialog.visible)
                dialog.refresh()
        }
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    component Field: ColumnLayout {
        id: field
        property string caption: ""
        property alias text: inputItem.text
        property string placeholder: ""
        property string fieldName: ""
        property int fieldWidth: Theme.space(52)
        width: field.fieldWidth
        spacing: Theme.space(1)
        Label { text: field.caption }
        FieldInput {
            id: inputItem
            objectName: "rightsInput_" + field.fieldName
            Layout.fillWidth: true
            Accessible.name: field.caption
            placeholderText: dialog.various[field.fieldName] === true ? qsTr("(various)") : field.placeholder
            // The dialog is a raised card: the field sits sunken in it, so its edge shows. A long text shows its start.
            background: Rectangle {
                radius: height / 2
                color: Theme.sunken
                border.width: inputItem.activeFocus ? Theme.focusRingWidth : Theme.hairline
                border.color: inputItem.activeFocus ? Theme.focus : Theme.border
            }
            onTextChanged: if (!inputItem.activeFocus) inputItem.cursorPosition = 0
            onTextEdited: dialog.markEdited(field.fieldName)
            onAccepted: if (dialog.editedCount > 0) dialog.apply()
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
        }
    }

    Flickable {
        id: scroller
        objectName: "rightsScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(bodyColumn.implicitHeight,
                                         Math.max(Theme.space(60), (dialog.parent ? dialog.parent.height : Theme.space(170)) - Theme.space(60)))
        contentWidth: width
        contentHeight: bodyColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: bodyColumn
            width: scroller.width - Theme.space(3)
            spacing: Theme.space(3)

            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(3)
                PillTabs {
                    id: scopeTabs
                    objectName: "rightsScopeTabs"
                    width: Theme.space(70)
                    compact: true
                    model: [qsTr("This sequence"), qsTr("Whole project")]
                    currentIndex: dialog.scope === "project" ? 1 : 0
                    Accessible.name: qsTr("Which sources")
                    onActivated: index => {
                        dialog.scope = index === 1 ? "project" : "sequence"
                        dialog.refresh()
                    }
                }
                Toggle {
                    objectName: "rightsFlaggedOnly"
                    text: qsTr("Needs attention only")
                    checked: dialog.flaggedOnly
                    onToggled: {
                        dialog.flaggedOnly = checked
                        dialog.refresh()
                    }
                }
                Row {
                    spacing: Theme.space(2)
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Judge on")
                    }
                    FieldInput {
                        id: asOfField
                        objectName: "rightsAsOf"
                        width: Theme.space(34)
                        text: dialog.asOf
                        placeholderText: qsTr("YYYY-MM-DD")
                        Accessible.name: qsTr("Date licences are judged on")
                        onEditingFinished: {
                            if (/^\d{4}-\d{2}-\d{2}$/.test(text)) {
                                dialog.asOf = text
                                dialog.refresh()
                            } else {
                                text = dialog.asOf
                            }
                        }
                        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                    }
                }
            }

            Text {
                objectName: "rightsSummary"
                Layout.fillWidth: true
                text: dialog.result.ok === true ? dialog.summaryText() : (dialog.result.error ?? "")
                color: dialog.result.ok === true ? Theme.textPrimary : Theme.dangerText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                wrapMode: Text.Wrap
            }

            // ---- The log ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(Theme.space(24), Math.min(rowList.contentHeight, Theme.space(70))) + Theme.space(2)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: rowList.activeFocus ? Theme.accentStroke : Theme.border

                ListView {
                    id: rowList
                    objectName: "rightsRows"
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    clip: true
                    spacing: Theme.space(0.5)
                    model: dialog.rows
                    currentIndex: -1
                    activeFocusOnTab: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar {}
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Sources and their rights")
                    Keys.onShortcutOverride: event => event.accepted = (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) || event.key === Qt.Key_Up || event.key === Qt.Key_Down
                    function step(delta: int, extend: bool) {
                        if (dialog.rows.length === 0)
                            return
                        const keys = dialog.rows.map(r => r.key)
                        const from = keys.indexOf(dialog.anchorKey)
                        const next = Math.max(0, Math.min(keys.length - 1, (from < 0 ? (delta > 0 ? -1 : keys.length) : from) + delta))
                        dialog.select(keys[next], false, false)
                        rowList.positionViewAtIndex(next, ListView.Contain)
                    }
                    Keys.onUpPressed: step(-1, false)
                    Keys.onDownPressed: step(1, false)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                            const all = ({})
                            for (const r of dialog.rows)
                                all[r.key] = true
                            dialog.selected = all
                            dialog.loadEditor()
                            event.accepted = true
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - Theme.space(8)
                        visible: dialog.rows.length === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: dialog.flaggedOnly
                              ? qsTr("Nothing needs attention on %1.").arg(dialog.asOf)
                              : dialog.scope === "sequence"
                                ? qsTr("The sequence uses no footage yet.")
                                : qsTr("The project has no footage yet.")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }

                    delegate: Rectangle {
                        id: rowItem
                        required property var modelData
                        required property int index
                        readonly property bool picked: dialog.selected[rowItem.modelData.key] === true
                        width: rowList.width - Theme.space(2)
                        implicitHeight: rowColumn.implicitHeight + Theme.space(3)
                        height: implicitHeight
                        radius: Theme.radiusField
                        color: rowItem.picked ? Theme.hover : rowMouse.containsMouse ? Qt.alpha(Theme.hover, 0.5) : "transparent"
                        border.width: rowItem.picked ? Theme.hairline : 0
                        border.color: Theme.accentStroke
                        objectName: "rightsRow_" + rowItem.index
                        Accessible.role: Accessible.ListItem
                        Accessible.name: rowItem.modelData.media + ", " + (rowItem.modelData.statusLabel || qsTr("no rights record"))
                                         + (rowItem.modelData.flags && rowItem.modelData.hasRecord ? ", " + rowItem.modelData.flags : "")
                        Accessible.selected: rowItem.picked

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: mouse => {
                                dialog.select(rowItem.modelData.key, (mouse.modifiers & Qt.ControlModifier) !== 0,
                                              (mouse.modifiers & Qt.ShiftModifier) !== 0)
                                rowList.forceActiveFocus()
                            }
                        }
                        ColumnLayout {
                            id: rowColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.space(3)
                            anchors.rightMargin: Theme.space(3)
                            spacing: Theme.space(1)
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space(2)
                                Text {
                                    Layout.fillWidth: true
                                    text: rowItem.modelData.media
                                    elide: Text.ElideMiddle
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    visible: (rowItem.modelData.uses ?? 0) > 0
                                    text: qsTr("%n use(s)", "", rowItem.modelData.uses ?? 0)
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                                StatusPill {
                                    visible: rowItem.modelData.hasRecord === true
                                    label: rowItem.modelData.statusLabel || qsTr("Status not set")
                                    dotColor: dialog.statusColor(rowItem.modelData.status ?? "")
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text !== ""
                                text: dialog.detailText(rowItem.modelData)
                                elide: Text.ElideRight
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: (rowItem.modelData.creditLine ?? "") !== ""
                                text: qsTr("Credit: %1").arg(rowItem.modelData.creditLine)
                                elide: Text.ElideRight
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text !== ""
                                text: rowItem.modelData.hasRecord === true ? (rowItem.modelData.flags ?? "") : qsTr("No rights record")
                                elide: Text.ElideRight
                                color: rowItem.modelData.hasRecord !== true ? Theme.textTertiary
                                       : (rowItem.modelData.uncleared || rowItem.modelData.expired) ? Theme.dangerText : Theme.warningText
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                }
            }

            // ---- The editor for the selected rows ----
            Text {
                objectName: "rightsEditorTitle"
                Layout.fillWidth: true
                text: dialog.selectedKeys.length > 0
                      ? qsTr("Rights of %n item(s)", "", dialog.selectedKeys.length)
                      : qsTr("Choose one or more sources above to set where they came from and whether they are cleared.")
                color: dialog.selectedKeys.length > 0 ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: dialog.selectedKeys.length > 0 ? Theme.textLg : Theme.textSm
                font.weight: dialog.selectedKeys.length > 0 ? Font.DemiBold : Font.Normal
                wrapMode: Text.Wrap
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.selectedKeys.length > 0
                spacing: Theme.space(2)
                Label { text: qsTr("Status") }
                PillTabs {
                    id: statusTabs
                    objectName: "rightsStatusTabs"
                    Layout.fillWidth: true
                    Layout.maximumWidth: Theme.space(150)
                    compact: true
                    model: dialog.statusNames
                    currentIndex: 0
                    Accessible.name: qsTr("Clearance status")
                    onActivated: dialog.markEdited("status")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(3)
                    Field { id: sourceField; objectName: "rightsSource"; caption: qsTr("Source"); fieldName: "source"; placeholder: qsTr("Archive or owner") }
                    Field { id: licenceField; objectName: "rightsLicence"; caption: qsTr("Licence"); fieldName: "licence"; placeholder: qsTr("Terms") }
                    Field { id: referenceField; objectName: "rightsReference"; caption: qsTr("Reference"); fieldName: "reference"; placeholder: qsTr("Licence or invoice number") }
                    Field { id: territoryField; objectName: "rightsTerritory"; caption: qsTr("Territory"); fieldName: "territory"; placeholder: qsTr("Worldwide") }
                    Field { id: expiryField; objectName: "rightsExpiry"; caption: qsTr("Expires"); fieldName: "expiry"; placeholder: qsTr("YYYY-MM-DD"); fieldWidth: Theme.space(40) }
                    Field { id: costField; objectName: "rightsCost"; caption: qsTr("Cost"); fieldName: "cost"; placeholder: qsTr("1250.00"); fieldWidth: Theme.space(40) }
                    Field { id: currencyField; objectName: "rightsCurrency"; caption: qsTr("Currency"); fieldName: "currency"; placeholder: qsTr("USD"); fieldWidth: Theme.space(26) }
                    Field { id: creditField; objectName: "rightsCredit"; caption: qsTr("Credit line"); fieldName: "creditLine"; placeholder: qsTr("As it must appear in the credits"); fieldWidth: Theme.space(100) }
                    Field { id: notesField; objectName: "rightsNotes"; caption: qsTr("Notes"); fieldName: "notes"; fieldWidth: Theme.space(100) }
                }
                Row {
                    spacing: Theme.space(3)
                    PillButton {
                        objectName: "rightsApply"
                        variant: "primary"
                        text: qsTr("Apply to %n item(s)", "", dialog.selectedKeys.length)
                        enabled: dialog.editedCount > 0
                        disabledReason: qsTr("Change a field first")
                        onClicked: dialog.apply()
                    }
                    PillButton {
                        objectName: "rightsRevert"
                        variant: "ghost"
                        text: qsTr("Revert")
                        enabled: dialog.editedCount > 0
                        onClicked: dialog.loadEditor()
                    }
                }
            }

            Text {
                objectName: "rightsError"
                Layout.fillWidth: true
                visible: text !== ""
                text: dialog.errorText
                color: Theme.dangerText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
            }
            Text {
                objectName: "rightsNotice"
                Layout.fillWidth: true
                visible: text !== ""
                text: dialog.noticeText
                color: Theme.success
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
            }
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        currentFolder: ExportController.lastFolderUrl
        defaultSuffix: "csv"
        nameFilters: [qsTr("CSV files (*.csv)"), qsTr("JSON files (*.json)")]
        onAccepted: {
            const out = Rights.writeLog(selectedFile.toString(), { scope: dialog.scope, flaggedOnly: dialog.flaggedOnly, asOf: dialog.asOf })
            if (out.ok !== true) {
                dialog.errorText = out.error ?? ""
                return
            }
            dialog.errorText = ""
            dialog.noticeText = qsTr("Log saved.")
            dialog.written(out.path, out.count ?? 0)
        }
    }

    footerData: [
        PillButton {
            objectName: "rightsExport"
            text: qsTr("Export log…")
            enabled: dialog.result.ok === true
            onClicked: saveDialog.open()
        },
        PillButton {
            objectName: "rightsCreditsRoll"
            text: qsTr("Make credits roll")
            enabled: ProjectController.sequenceNames.length > 0
            disabledReason: qsTr("Open a sequence first")
            onClicked: {
                const made = Rights.makeCreditsRoll({})
                if (made.ok !== true) {
                    dialog.errorText = made.error ?? ""
                    return
                }
                dialog.close()
                dialog.rollMade((made.credits ?? []).length, (made.missingCreditLine ?? []).length)
            }
        },
        PillButton {
            objectName: "rightsClose"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
