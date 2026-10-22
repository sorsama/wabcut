pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P.2 (E6): Premiere's View > Guide Templates (bridge Monitors): "Save Guides as Template…" (`mode` "save": a name for
// the current guides; a name in use is replaced) and "Manage Guides…" (`mode` "manage": the templates, to apply, delete,
// export as a guides file or import from one). Templates belong to the user, like the guides.
WabcutDialog {
    id: dialog
    objectName: "guideTemplatesDialog"

    property string mode: "manage"
    property string selected: ""
    property string errorText: ""

    function openSave() {
        dialog.mode = "save"
        nameField.text = ""
        dialog.errorText = Monitors.guides.length === 0 ? qsTr("There are no guides to save") : ""
        open()
        nameField.forceActiveFocus()
    }
    function openManage() {
        dialog.mode = "manage"
        dialog.selected = Monitors.guideTemplates.length > 0 ? Monitors.guideTemplates[0] : ""
        dialog.errorText = ""
        open()
    }
    function save() {
        const error = Monitors.saveGuidesAsTemplate(nameField.text)
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: dialog.mode === "save" ? qsTr("Save Guides as Template") : qsTr("Manage Guides")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    // ---- Save ----
    FieldInput {
        id: nameField
        objectName: "guideTemplateName"
        Layout.fillWidth: true
        visible: dialog.mode === "save"
        placeholderText: qsTr("Template name")
        Accessible.name: qsTr("Template name")
        onAccepted: dialog.save()
    }

    // ---- Manage ----
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.space(48)
        visible: dialog.mode === "manage"
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border

        ListView {
            id: list
            objectName: "guideTemplateList"
            anchors.fill: parent
            anchors.margins: Theme.space(1)
            clip: true
            model: Monitors.guideTemplates
            delegate: Rectangle {
                id: entry
                required property string modelData
                width: ListView.view.width
                height: Theme.controlHeight
                radius: Theme.radiusField
                color: dialog.selected === modelData ? Theme.accentWash : "transparent"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(2)
                    text: entry.modelData
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: dialog.selected = entry.modelData
                    onDoubleClicked: {
                        Monitors.applyGuideTemplate(entry.modelData)
                        dialog.close()
                    }
                }
            }
        }
        Text {
            anchors.centerIn: parent
            visible: list.count === 0
            text: qsTr("No guide templates yet")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
    RowLayout {
        visible: dialog.mode === "manage"
        spacing: Theme.space(2)
        PillButton {
            objectName: "guideTemplateApply"
            text: qsTr("Apply")
            enabled: dialog.selected !== ""
            onClicked: {
                Monitors.applyGuideTemplate(dialog.selected)
                dialog.close()
            }
        }
        PillButton {
            objectName: "guideTemplateDelete"
            variant: "ghost"
            text: qsTr("Delete")
            enabled: dialog.selected !== ""
            onClicked: {
                Monitors.removeGuideTemplate(dialog.selected)
                dialog.selected = Monitors.guideTemplates.length > 0 ? Monitors.guideTemplates[0] : ""
            }
        }
        PillButton {
            variant: "ghost"
            text: qsTr("Export…")
            enabled: dialog.selected !== ""
            onClicked: exportDialog.open()
        }
        PillButton {
            variant: "ghost"
            text: qsTr("Import…")
            onClicked: importDialog.open()
        }
    }

    Text {
        objectName: "guideTemplateError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: dialog.mode === "save" ? qsTr("Cancel") : qsTr("Close")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "guideTemplateSave"
            visible: dialog.mode === "save"
            variant: "primary"
            text: qsTr("Save")
            enabled: Monitors.guides.length > 0
            onClicked: dialog.save()
        }
    ]

    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "guides"
        nameFilters: [qsTr("Guides (*.guides)")]
        onAccepted: dialog.errorText = Monitors.exportGuideTemplates([dialog.selected], selectedFile)
    }
    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Guides (*.guides)")]
        onAccepted: dialog.errorText = Monitors.importGuideTemplates(selectedFile)
    }
}
