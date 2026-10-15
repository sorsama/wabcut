pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A form an extension opened (C.12 wabcut.ui.prompt, M5n; an entry of Extensions.prompts): a non-modal dialog titled
// "<extension>: <title>" with the message above the fields. Fields: text (a line, or `multiline` an area; `readOnly`
// shows a result to select with a Copy button), number (min, max, step), checkbox, choice (options). Submit answers
// with every field's value (Extensions.answerPrompt; answers outside the form's bounds keep it open); Cancel or Esc
// answers null. The extension waits without a time limit while it is open.
WabcutDialog {
    id: dialog
    objectName: "extensionPrompt"

    /// The form: { key, extensionId, extensionName, title, message, submitLabel, cancelLabel, fields }.
    property var prompt: ({})
    /// Field id -> value.
    property var values: ({})
    property bool answered: false
    property string error: ""

    function setValue(id, value) {
        const v = Object.assign({}, dialog.values)
        v[id] = value
        dialog.values = v
        dialog.error = ""
    }
    function submit() {
        if (Extensions.answerPrompt(dialog.prompt.key, dialog.values)) {
            dialog.answered = true
            dialog.close()
        } else {
            dialog.error = qsTr("Check the values: one is outside what this form accepts.")
        }
    }

    title: qsTr("%1: %2").arg(prompt.extensionName ?? "").arg(prompt.title ?? "")
    modal: false
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))
    onClosed: {
        if (!dialog.answered && (dialog.prompt.key ?? "") !== "")
            Extensions.cancelPrompt(dialog.prompt.key)
    }
    Component.onCompleted: {
        const v = {}
        for (const f of (dialog.prompt.fields ?? []))
            v[f.id] = f.value
        dialog.values = v
    }

    component FieldLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Text {
        objectName: "extensionPromptMessage"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.prompt.message ?? ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    Repeater {
        model: dialog.prompt.fields ?? []
        delegate: ColumnLayout {
            id: field
            required property var modelData
            readonly property string fieldId: modelData.id
            readonly property string type: modelData.type
            readonly property bool multiline: modelData.multiline === true
            readonly property bool readOnly: modelData.readOnly === true
            objectName: "extensionField_" + fieldId
            Layout.fillWidth: true
            spacing: Theme.space(1)

            FieldLabel {
                visible: field.type !== "checkbox" && text !== ""
                text: field.modelData.label ?? ""
                textFormat: Text.PlainText
            }

            // text: one line
            FieldInput {
                objectName: "extensionInput_" + field.fieldId
                Layout.fillWidth: true
                visible: field.type === "text" && !field.multiline && !field.readOnly
                text: String(field.modelData.value ?? "")
                placeholderText: field.modelData.placeholder ?? ""
                maximumLength: field.modelData.maxLength > 0 ? field.modelData.maxLength : 32767
                Accessible.name: field.modelData.label ?? field.fieldId
                onTextChanged: {
                    if (field.type === "text" && !field.multiline && !field.readOnly)
                        dialog.setValue(field.fieldId, text)
                }
            }
            // text: an area (multiline), or a result to copy (readOnly)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(field.readOnly ? 36 : 24)
                visible: field.type === "text" && (field.multiline || field.readOnly)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: area.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: area
                        objectName: "extensionArea_" + field.fieldId
                        text: String(field.modelData.value ?? "")
                        readOnly: field.readOnly
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: Theme.textPrimary
                        font.family: field.readOnly ? Theme.fontMono : Theme.fontSans
                        font.pixelSize: Theme.textSm
                        placeholderText: field.modelData.placeholder ?? ""
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        Accessible.name: field.modelData.label ?? field.fieldId
                        onTextChanged: {
                            if (field.type === "text" && field.multiline && !field.readOnly)
                                dialog.setValue(field.fieldId, text)
                        }
                    }
                }
            }
            PillButton {
                objectName: "extensionCopy_" + field.fieldId
                Layout.alignment: Qt.AlignRight
                visible: field.type === "text" && field.readOnly
                compact: true
                iconName: "copy"
                text: qsTr("Copy")
                onClicked: {
                    Extensions.copyText(area.text)
                    text = qsTr("Copied")
                }
            }
            // number
            NumberField {
                objectName: "extensionNumber_" + field.fieldId
                Layout.preferredWidth: Theme.numberFieldWidth * 1.5
                visible: field.type === "number"
                from: field.modelData.min ?? -1e9
                to: field.modelData.max ?? 1e9
                stepSize: field.modelData.step ?? 1
                decimals: (field.modelData.step ?? 1) < 1 ? 2 : 0
                value: Number(dialog.values[field.fieldId] ?? field.modelData.value ?? 0)
                Accessible.name: field.modelData.label ?? field.fieldId
                onValueModified: v => dialog.setValue(field.fieldId, v)
            }
            // checkbox
            Toggle {
                objectName: "extensionCheck_" + field.fieldId
                visible: field.type === "checkbox"
                text: field.modelData.label ?? ""
                checked: dialog.values[field.fieldId] === true
                onToggled: dialog.setValue(field.fieldId, checked)
            }
            // choice
            PillButton {
                id: choiceButton
                objectName: "extensionChoice_" + field.fieldId
                Layout.fillWidth: true
                visible: field.type === "choice"
                compact: true
                iconName: "chevron-down"
                readonly property var options: field.modelData.options ?? []
                readonly property var current: options.find(o => o.value === dialog.values[field.fieldId])
                text: current ? current.label : ""
                Accessible.name: field.modelData.label ?? field.fieldId
                onClicked: choiceMenu.popup(choiceButton, 0, choiceButton.height + Theme.space(1))
                OptionsMenu {
                    id: choiceMenu
                    options: choiceButton.options.map(o => ({ id: String(o.value), name: o.label }))
                    current: String(dialog.values[field.fieldId] ?? "")
                    onChosen: id => {
                        const option = choiceButton.options.find(o => String(o.value) === id)
                        if (option)
                            dialog.setValue(field.fieldId, option.value)
                    }
                }
            }
        }
    }

    Text {
        objectName: "extensionPromptError"
        Layout.fillWidth: true
        visible: dialog.error !== ""
        text: dialog.error
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "extensionPromptCancel"
            variant: "ghost"
            text: dialog.prompt.cancelLabel || qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "extensionPromptSubmit"
            variant: "primary"
            text: dialog.prompt.submitLabel || qsTr("OK")
            onClicked: dialog.submit()
        }
    ]
}
