pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Save as template… (Essential Graphics, M5o): keeps the selected graphic as a motion graphics template (template.save)
// in the project's library or the user's, with the fields an editor of an instance sees: each layer's text, font,
// colours, picture, out point and visibility, and the clip's length, as rows to tick and name (Graphics.exposeCandidates).
WabcutDialog {
    id: dialog
    objectName: "graphicTemplateDialog"

    function openForSelection() {
        nameField.text = (Graphics.graphic.name ?? "") !== "" ? Graphics.graphic.name : qsTr("Lower third")
        descriptionField.text = ""
        libraryTabs.currentIndex = 0
        errorText.text = ""
        candidates.clear()
        const list = Graphics.exposeCandidates()
        for (let i = 0; i < list.length; ++i) {
            const c = list[i]
            candidates.append({ fieldId: c.id, label: c.label, kind: c.kind, layer: c.layer, prop: c.property,
                                layerName: c.layerName, exposed: c.kind === "text" })
        }
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    /// The ticked rows as template.save fields.
    function exposedFields() {
        const out = []
        for (let i = 0; i < candidates.count; ++i) {
            const c = candidates.get(i)
            if (c.exposed)
                out.push({ id: c.fieldId, label: c.label, kind: c.kind, layer: c.layer, property: c.prop })
        }
        return out
    }
    function accept() {
        const error = Graphics.saveTemplate("", nameField.text.trim(), descriptionField.text.trim(),
                                            libraryTabs.currentIndex === 1 ? "user" : "project", dialog.exposedFields())
        if (error === "")
            dialog.close()
        else
            errorText.text = error
    }

    title: qsTr("Save as template")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    ListModel {
        id: candidates
    }

    FieldInput {
        id: nameField
        objectName: "graphicTemplateName"
        Layout.fillWidth: true
        placeholderText: qsTr("Name")
        Accessible.name: qsTr("Template name")
        onAccepted: dialog.accept()
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    FieldInput {
        id: descriptionField
        objectName: "graphicTemplateDescription"
        Layout.fillWidth: true
        placeholderText: qsTr("Description (optional)")
        Accessible.name: qsTr("Template description")
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
    }
    PillTabs {
        id: libraryTabs
        objectName: "graphicTemplateLibrary"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("This project"), qsTr("Your library (every project)")]
        Accessible.name: qsTr("Where to keep the template")
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Fields people can edit")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    ListView {
        id: fieldList
        objectName: "graphicTemplateFields"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(80))
        clip: true
        model: candidates
        spacing: Theme.space(0.5)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Fields people can edit")

        delegate: RowLayout {
            id: candidateRow
            required property int index
            required property string fieldId
            required property string label
            required property string kind
            required property string layerName
            required property bool exposed
            width: fieldList.width - Theme.space(3)
            spacing: Theme.space(2)
            Toggle {
                objectName: "graphicExpose_" + candidateRow.fieldId
                checked: candidateRow.exposed
                Accessible.name: qsTr("Expose %1").arg(candidateRow.label)
                onToggled: candidates.setProperty(candidateRow.index, "exposed", checked)
            }
            FieldInput {
                objectName: "graphicExposeLabel_" + candidateRow.fieldId
                Layout.fillWidth: true
                enabled: candidateRow.exposed
                text: candidateRow.label
                Accessible.name: qsTr("Label of %1").arg(candidateRow.label)
                onEditingFinished: {
                    if (text.trim() !== "" && text !== candidateRow.label)
                        candidates.setProperty(candidateRow.index, "label", text.trim())
                }
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            Text {
                Layout.preferredWidth: Theme.space(18)
                text: ({ text: qsTr("Text"), font: qsTr("Font"), color: qsTr("Colour"), image: qsTr("Picture"),
                         duration: qsTr("Length"), checkbox: qsTr("On / off") })[candidateRow.kind] ?? candidateRow.kind
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
    Text {
        id: errorText
        objectName: "graphicTemplateError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "graphicTemplateSave"
            variant: "primary"
            text: qsTr("Save template")
            enabled: nameField.text.trim() !== ""
            onClicked: dialog.accept()
        }
    ]
}
