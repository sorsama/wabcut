pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// ● METADATA (0.2, P.3: Premiere's Metadata panel) for the Library's selected media item.
//   * Clip: what the project keeps about the item - its type and format (read only), then Tape Name, Description,
//     Comment, Log Note, Scene, Shot, Take, Client, Status, the camera fields (Camera Angle / Label / Model, Shot Date and
//     Location) and Good, edited here (one undo step per field; Enter or leaving the field commits). With several items
//     selected an edit applies to all of them, as in Premiere.
//   * File: the XMP its file carries (a sidecar "<name>.xmp" first, else the packet inside the file), grouped by schema
//     (Dublin Core, Dynamic Media, ...), and the tags of its container and streams. XMP properties are edited here and
//     "Add property" sets one the file does not have yet: into the sidecar, or into the MP4 / MOV itself when
//     Preferences > Media says so (Library.setFileMetadata). Linked properties change the clip fields too. "Read from
//     file" fills the clip fields from the XMP (Premiere's links: Scene = xmpDM:scene, Shot = xmpDM:shotName, ...);
//     "Write XMP" writes them out. Container tags are read only.
Panel {
    id: root

    objectName: "metadataPanel"
    title: qsTr("Metadata")

    /// The media items selected in the Library; the first is shown.
    readonly property var mediaKeys: Library.selection.filter(k => k.startsWith("m"))
    readonly property string key: mediaKeys.length > 0 ? mediaKeys[0] : ""
    readonly property var clipData: ProjectController.revision >= 0 && key !== "" ? Library.clipMetadata(key) : ({})
    property var file: ({})
    property string search: ""
    /// The property being added (from the Add property menu), or null.
    property var adding: null
    /// The Scene field, which gets the focus (focusContent).
    property MetadataField sceneField: null

    function focusContent() {
        if (root.key !== "" && root.sceneField)
            root.sceneField.focusField()
        else
            searchField.forceActiveFocus(Qt.TabFocusReason)
    }
    function reloadFile() {
        root.file = root.key !== "" ? Library.fileMetadata(root.key) : ({})
    }
    function setFileProperty(ns, prefix, property, value) {
        if (root.key !== "" && Library.setFileMetadata(root.key, ns, prefix, property, value))
            root.reloadFile()
    }
    function matches(label, value) {
        const s = root.search.trim().toLowerCase()
        return s === "" || String(label).toLowerCase().includes(s) || String(value).toLowerCase().includes(s)
    }
    onKeyChanged: {
        root.adding = null
        reloadFile()
    }
    Component.onCompleted: reloadFile()

    Connections {
        target: Library
        function onFileMetadataChanged(changed) {
            if (changed === root.key)
                root.reloadFile()
        }
    }

    trailing: [
        IconButton {
            objectName: "metadataReadXmpButton"
            compact: true
            iconName: "import"
            tooltip: qsTr("Read clip metadata from the files' XMP")
            enabled: root.key !== ""
            onClicked: {
                Library.readXmp(root.mediaKeys)
                root.reloadFile()
            }
        },
        IconButton {
            objectName: "metadataWriteXmpButton"
            compact: true
            iconName: "save"
            tooltip: qsTr("Write clip metadata into XMP sidecars")
            enabled: root.key !== "" && (root.clipData.hasFile ?? false)
            onClicked: {
                Library.writeXmp(root.mediaKeys)
                root.reloadFile()
            }
        }
    ]

    FieldInput {
        id: searchField
        objectName: "metadataSearch"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        iconName: "search"
        clearOnEscape: true
        placeholderText: qsTr("Search metadata")
        onTextChanged: root.search = text
        Accessible.name: qsTr("Search metadata")
    }

    EmptyState {
        objectName: "metadataEmpty"
        anchors.centerIn: parent
        width: parent.width - Theme.space(8)
        visible: root.key === ""
        illustration: "library"
        text: qsTr("Select a clip in the Library to see and edit its metadata.")
    }

    Flickable {
        id: flick
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchField.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(2)
        visible: root.key !== ""
        clip: true
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: content
            width: flick.width
            spacing: Theme.space(1.5)

            SectionHeader {
                text: root.mediaKeys.length > 1 ? qsTr("Clip: %1 (and %n more)", "", root.mediaKeys.length - 1).arg(root.clipData.name ?? "")
                                                : qsTr("Clip: %1").arg(root.clipData.name ?? "")
            }

            // Read-only facts.
            Repeater {
                model: [
                    { label: qsTr("Media Type"), value: root.clipData.type ?? "" },
                    { label: qsTr("Label"), value: root.clipData.labelName ?? "" },
                    { label: qsTr("Frame Rate"), value: root.clipData.fps ?? "" },
                    { label: qsTr("Media Duration"), value: root.clipData.duration ?? "" },
                    { label: qsTr("Video Info"), value: root.clipData.resolution ?? "" },
                    { label: qsTr("Audio Info"), value: root.clipData.audio ?? "" },
                    { label: qsTr("File Path"), value: root.clipData.path ?? "" },
                    { label: qsTr("Imported From"), value: root.clipData.importedFrom ?? "" }
                ]
                delegate: Item {
                    id: fact
                    required property var modelData
                    width: content.width
                    height: visible ? Math.max(factValue.implicitHeight, factLabel.implicitHeight) : 0
                    visible: fact.modelData.value !== "" && root.matches(fact.modelData.label, fact.modelData.value)
                    Text {
                        id: factLabel
                        width: Math.round(parent.width * 0.38)
                        text: fact.modelData.label
                        color: Theme.textTertiary
                        elide: Text.ElideRight
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        id: factValue
                        anchors.left: factLabel.right
                        anchors.right: parent.right
                        anchors.leftMargin: Theme.space(2)
                        text: fact.modelData.value
                        color: Theme.textSecondary
                        wrapMode: Text.Wrap
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
            }

            // Editable clip metadata.
            Repeater {
                model: [
                    { field: "tapeName", label: qsTr("Tape Name") },
                    { field: "description", label: qsTr("Description") },
                    { field: "comment", label: qsTr("Comment") },
                    { field: "logNote", label: qsTr("Log Note") },
                    { field: "scene", label: qsTr("Scene") },
                    { field: "shot", label: qsTr("Shot") },
                    { field: "take", label: qsTr("Take") },
                    { field: "client", label: qsTr("Client") },
                    { field: "status", label: qsTr("Status") },
                    { field: "cameraAngle", label: qsTr("Camera Angle") },
                    { field: "cameraLabel", label: qsTr("Camera Label") },
                    { field: "cameraModel", label: qsTr("Camera Model") },
                    { field: "shotDate", label: qsTr("Shot Date") },
                    { field: "shotLocation", label: qsTr("Shot Location") }
                ]
                delegate: MetadataField {
                    id: fieldDelegate
                    required property var modelData
                    objectName: "metadata_" + modelData.field
                    width: content.width
                    fieldName: modelData.field
                    label: modelData.label
                    current: root.clipData[modelData.field] ?? ""
                    shown: root.matches(modelData.label, current)
                    onCommitted: value => {
                        if (root.key !== "")
                            Library.setMetadata(root.mediaKeys, fieldName, value)
                    }
                    Component.onCompleted: if (fieldName === "scene") root.sceneField = fieldDelegate
                }
            }
            Item {
                width: content.width
                height: visible ? goodToggle.implicitHeight : 0
                visible: root.matches(qsTr("Good"), "")
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(parent.width * 0.38)
                    text: qsTr("Good")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Toggle {
                    id: goodToggle
                    objectName: "metadataGood"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.clipData.good ?? false
                    tooltip: qsTr("Good (a circled take)")
                    onToggled: {
                        Library.setMetadata(root.mediaKeys, "good", checked)
                        checked = Qt.binding(() => root.clipData.good ?? false)
                    }
                }
            }

            // ---- File ----
            Item {
                width: content.width
                height: fileHeader.implicitHeight
                SectionHeader {
                    id: fileHeader
                    anchors.left: parent.left
                    text: (root.file.xmpSource ?? "") === "sidecar" ? qsTr("File: XMP sidecar")
                          : (root.file.xmpSource ?? "") === "embedded" ? qsTr("File: XMP in the file") : qsTr("File")
                }
                IconButton {
                    id: addPropertyButton
                    objectName: "metadataAddProperty"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "plus"
                    tooltip: qsTr("Add property")
                    enabled: (root.clipData.hasFile ?? false) && !(root.clipData.offline ?? false)
                    onClicked: propertyMenu.openFor(addPropertyButton)
                }
            }
            Text {
                width: content.width
                visible: text !== ""
                text: root.file.writesToText ?? ""
                color: Theme.textTertiary
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            MetadataField {
                id: addingField
                objectName: "metadataAdding"
                width: content.width
                shown: root.adding !== null
                label: root.adding ? root.adding.label : ""
                current: ""
                onCommitted: value => {
                    const choice = root.adding
                    root.adding = null
                    if (choice && value !== "")
                        root.setFileProperty(choice.ns, choice.prefix, choice.property, value)
                }
            }
            Text {
                width: content.width
                visible: text !== ""
                text: (root.file.error ?? "") !== "" ? root.file.error
                      : (root.file.xmp ?? []).length === 0 && (root.file.tags ?? []).length === 0 ? qsTr("No metadata in the file.") : ""
                color: Theme.textTertiary
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Repeater {
                model: root.file.xmp ?? []
                delegate: MetadataRow {
                    required property var modelData
                    required property int index
                    objectName: "metadataXmp_" + index
                    width: content.width
                    group: modelData.group
                    // `?.`: the file shown may go (an undone import) before this row does.
                    showGroup: index === 0 || (root.file.xmp?.[index - 1]?.group !== modelData.group)
                    label: modelData.label
                    value: modelData.value
                    hint: modelData.name
                    editable: (modelData.editable ?? false) && !(root.clipData.offline ?? false)
                    visible: root.matches(modelData.label, modelData.value)
                    onEdited: text => root.setFileProperty(modelData.ns, modelData.prefix, modelData.property, text)
                }
            }
            Repeater {
                model: root.file.tags ?? []
                delegate: MetadataRow {
                    required property var modelData
                    required property int index
                    objectName: "metadataTag_" + index
                    width: content.width
                    group: modelData.scope
                    showGroup: index === 0 || (root.file.tags?.[index - 1]?.scope !== modelData.scope)
                    label: modelData.key
                    value: modelData.value
                    visible: root.matches(modelData.key, modelData.value)
                }
            }
        }
    }

    // One editable clip field: label and a text field; commits on Enter or when focus leaves.
    component MetadataField: Item {
        id: fieldItem
        property string fieldName: ""
        property string label: ""
        property string current: ""
        property bool shown: true
        signal committed(string value)
        height: visible ? input.implicitHeight : 0
        visible: shown

        function focusField() {
            input.forceActiveFocus(Qt.TabFocusReason)
        }
        function commit() {
            if (input.text !== fieldItem.current)
                fieldItem.committed(input.text)
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(parent.width * 0.38)
            text: fieldItem.label
            color: Theme.textTertiary
            elide: Text.ElideRight
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        FieldInput {
            id: input
            objectName: fieldItem.objectName + "Field"
            anchors.right: parent.right
            width: Math.round(parent.width * 0.6)
            text: fieldItem.current
            Accessible.name: fieldItem.label
            onAccepted: fieldItem.commit()
            onActiveFocusChanged: if (!activeFocus) fieldItem.commit()
            Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
        }
    }

    MetadataPropertyMenu {
        id: propertyMenu
        onChosen: choice => {
            root.adding = choice
            addingField.focusField()
        }
    }

    // One property of the file, under its group's name: edited in place when `editable` (an empty value removes it).
    component MetadataRow: Column {
        id: row
        property string group: ""
        property bool showGroup: false
        property string label: ""
        property string value: ""
        property string hint: ""
        property bool editable: false
        signal edited(string text)
        spacing: Theme.space(0.5)
        Text {
            visible: row.showGroup
            text: row.group
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
            topPadding: Theme.space(1)
        }
        Item {
            width: row.width
            height: Math.max(rowLabel.implicitHeight, row.editable ? rowInput.implicitHeight : rowValue.implicitHeight)
            Text {
                id: rowLabel
                width: Math.round(parent.width * 0.38)
                text: row.label
                color: Theme.textTertiary
                elide: Text.ElideRight
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                ToolTip.visible: rowHover.hovered && row.hint !== ""
                ToolTip.text: row.hint
                HoverHandler {
                    id: rowHover
                }
            }
            Text {
                id: rowValue
                anchors.left: rowLabel.right
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(2)
                visible: !row.editable
                text: row.value
                color: Theme.textSecondary
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 6
                elide: Text.ElideRight
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            FieldInput {
                id: rowInput
                objectName: row.objectName + "Field"
                anchors.right: parent.right
                width: Math.round(parent.width * 0.6)
                visible: row.editable
                text: row.value
                Accessible.name: row.label
                function commitEdit() {
                    if (row.editable && text !== row.value)
                        row.edited(text)
                }
                onAccepted: commitEdit()
                onActiveFocusChanged: if (!activeFocus) commitEdit()
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
        }
    }
}
