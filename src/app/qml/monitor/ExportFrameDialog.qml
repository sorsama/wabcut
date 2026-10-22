pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P.2 (E6): Premiere's Export Frame dialog (the camera in both monitors, Ctrl+Shift+E; bridge Monitors;
// docs/monitors.md): the name, the format (DPX, JPEG, PNG, TIFF, Targa, BMP, GIF, OpenEXR), the folder and "Import
// into project". The Program Monitor exports the sequence frame at the playhead, the Source Monitor its clip's frame.
WabcutDialog {
    id: dialog
    objectName: "exportFrameDialog"

    property bool source: false
    property var defaults: ({})
    property string format: "png"
    property url folder
    property string errorText: ""

    function openFor(fromSource) {
        dialog.source = fromSource
        dialog.defaults = Monitors.exportFrameDefaults(fromSource)
        nameField.text = dialog.defaults.name ?? ""
        dialog.format = dialog.defaults.format ?? "png"
        dialog.folder = dialog.defaults.folder
        importToggle.checked = dialog.defaults.importIntoProject === true
        dialog.errorText = dialog.defaults.available === true ? ""
                         : (fromSource ? qsTr("Load a clip with a picture in the Source Monitor") : qsTr("Open a sequence first"))
        open()
        nameField.forceActiveFocus()
    }
    function formatLabel(value) {
        const formats = dialog.defaults.formats ?? []
        for (let i = 0; i < formats.length; ++i) {
            if (formats[i].value === value)
                return formats[i].label
        }
        return value
    }
    function exportNow() {
        const written = Monitors.exportFrame(dialog.source, nameField.text, dialog.format, dialog.folder, importToggle.checked)
        if (written !== "")
            dialog.close()
        else
            dialog.errorText = Monitors.message
    }

    title: qsTr("Export Frame")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(3)

        Text {
            text: qsTr("Name")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        FieldInput {
            id: nameField
            objectName: "exportFrameName"
            Layout.fillWidth: true
            Accessible.name: qsTr("File name")
            onAccepted: dialog.exportNow()
        }
        Text {
            text: qsTr("Format")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        PillButton {
            id: formatButton
            objectName: "exportFrameFormat"
            Layout.alignment: Qt.AlignLeft
            text: dialog.formatLabel(dialog.format)
            iconName: "chevron-down"
            onClicked: formatMenu.popup(formatButton, 0, formatButton.height + Theme.space(1))

            ContextMenu {
                id: formatMenu
                Instantiator {
                    model: dialog.defaults.formats ?? []
                    delegate: ContextMenuItem {
                        required property var modelData
                        objectName: "exportFrameFormat_" + modelData.value
                        text: modelData.label
                        checkable: true
                        checked: dialog.format === modelData.value
                        onTriggered: dialog.format = modelData.value
                    }
                    onObjectAdded: (index, object) => formatMenu.insertItem(index, object as ContextMenuItem)
                    onObjectRemoved: (index, object) => formatMenu.removeItem(object as ContextMenuItem)
                }
            }
        }
        Text {
            text: qsTr("Path")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "exportFrameFolder"
                Layout.fillWidth: true
                text: decodeURIComponent(dialog.folder.toString().replace(/^file:\/\/\/?/, ""))
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                variant: "ghost"
                compact: true
                text: qsTr("Browse…")
                onClicked: folderDialog.open()
            }
        }
    }
    Toggle {
        id: importToggle
        objectName: "exportFrameImport"
        text: qsTr("Import into project")
    }
    Text {
        objectName: "exportFrameError"
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
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "exportFrameOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.defaults.available === true && nameField.text.trim() !== ""
            onClicked: dialog.exportNow()
        }
    ]

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder
        onAccepted: dialog.folder = selectedFolder
    }
}
