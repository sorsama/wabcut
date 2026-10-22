pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// New Search Bin / Edit Search Bin (0.2, P.3: Premiere's search bins): a name, what to find, where to look (all
// properties or one: name, comment, description, scene, shot, take, log note, client, tape name, path) and the same
// filters as Find (Good, offline, AI-generated, unused, the item type). A search bin lists, live, what its query finds
// in the whole project.
Popover {
    id: root
    objectName: "librarySearchBinPopover"

    /// "" = a new search bin, else the search bin being edited.
    property string key: ""
    property var query: ({})

    title: key === "" ? qsTr("New search bin") : qsTr("Edit search bin")

    function openNew(target) {
        key = ""
        query = { text: Library.findText, field: "", type: "", good: false, offline: false, generated: false,
                  unused: false }
        nameField.text = Library.findText
        findField.text = Library.findText
        parent = target
        open()
        nameField.forceActiveFocus()
    }
    function openEdit(binKey, target) {
        key = binKey
        query = Object.assign({}, Library.searchOf(binKey))
        nameField.text = Library.details(binKey).name ?? ""
        findField.text = query.text ?? ""
        parent = target
        open()
        findField.forceActiveFocus()
    }
    function set(name, value) {
        const next = Object.assign({}, root.query)
        next[name] = value
        root.query = next
    }
    function accept() {
        const q = Object.assign({}, root.query)
        q.text = findField.text
        if (root.key === "")
            Library.newSearchBin(nameField.text, q)
        else
            Library.editSearchBin(root.key, q)
        root.close()
    }

    component Chip: AbstractButton {
        id: chip
        property bool on: false
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        implicitHeight: Theme.controlHeightSmall
        implicitWidth: chipText.implicitWidth + Theme.space(5)
        Accessible.role: Accessible.CheckBox
        Accessible.checked: on
        contentItem: Text {
            id: chipText
            text: chip.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: chip.on ? Theme.accentOnFill : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        background: Rectangle {
            radius: Theme.radiusPill
            color: chip.on ? Theme.accentFill : chip.hovered ? Theme.hover : Theme.sunken
            border.width: Theme.hairline
            border.color: chip.on ? Theme.accentFill : Theme.border
            FocusRing {
                targetRadius: Theme.radiusPill
                visible: chip.visualFocus
            }
        }
    }
    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(84)
        spacing: Theme.space(2)

        FieldInput {
            id: nameField
            objectName: "searchBinName"
            width: parent.width
            placeholderText: qsTr("Search bin name")
            Accessible.name: qsTr("Search bin name")
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            onAccepted: root.accept()
        }
        FieldInput {
            id: findField
            objectName: "searchBinFind"
            width: parent.width
            iconName: "search"
            placeholderText: qsTr("Find")
            Accessible.name: qsTr("Find")
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            onAccepted: root.accept()
        }
        GroupLabel { text: qsTr("Search in") }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Repeater {
                model: Library.searchFields
                delegate: Chip {
                    required property var modelData
                    objectName: "searchBinField_" + (modelData.id === "" ? "all" : modelData.id)
                    text: modelData.title
                    on: (root.query.field ?? "") === modelData.id
                    onClicked: root.set("field", modelData.id)
                }
            }
        }
        GroupLabel { text: qsTr("Only") }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Repeater {
                model: [
                    { name: "good", text: qsTr("Good") },
                    { name: "offline", text: qsTr("Offline") },
                    { name: "generated", text: qsTr("AI-generated") },
                    { name: "unused", text: qsTr("Unused") },
                    { name: "transcribed", text: qsTr("Transcribed") }
                ]
                delegate: Chip {
                    required property var modelData
                    objectName: "searchBinOnly_" + modelData.name
                    text: modelData.text
                    on: root.query[modelData.name] ?? false
                    onClicked: root.set(modelData.name, !(root.query[modelData.name] ?? false))
                }
            }
        }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Repeater {
                model: [
                    { type: "", text: qsTr("Any type") },
                    { type: "video", text: qsTr("Video") },
                    { type: "audio", text: qsTr("Audio") },
                    { type: "still", text: qsTr("Stills") },
                    { type: "sequence", text: qsTr("Sequences") },
                    { type: "subclip", text: qsTr("Subclips") }
                ]
                delegate: Chip {
                    required property var modelData
                    objectName: "searchBinType_" + (modelData.type === "" ? "any" : modelData.type)
                    text: modelData.text
                    on: (root.query.type ?? "") === modelData.type
                    onClicked: root.set("type", modelData.type)
                }
            }
        }
        GroupLabel { text: qsTr("Label") }
        Flow {
            width: parent.width
            spacing: Theme.space(0.5)
            Chip {
                objectName: "searchBinLabel_any"
                text: qsTr("Any")
                on: (root.query.label ?? -1) < 0
                onClicked: root.set("label", -1)
            }
            Repeater {
                model: Library.labelNames
                delegate: Swatch {
                    required property string modelData
                    required property int index
                    objectName: "searchBinLabel_" + index
                    swatchColor: Theme.clipColor(index)
                    tooltip: modelData
                    checked: (root.query.label ?? -1) === index
                    onClicked: {
                        checked = Qt.binding(() => (root.query.label ?? -1) === index)
                        root.set("label", (root.query.label ?? -1) === index ? -1 : index)
                    }
                }
            }
        }
        Row {
            spacing: Theme.space(2)
            PillButton {
                objectName: "searchBinAccept"
                variant: "primary"
                text: root.key === "" ? qsTr("Create") : qsTr("Save")
                onClicked: root.accept()
            }
            PillButton {
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
