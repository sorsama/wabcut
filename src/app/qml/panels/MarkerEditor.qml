pragma ComponentBehavior: Bound
import QtQuick

// Edits one marker in place (M5i; double-click a marker in the ruler or the Markers panel): name, comment, In and
// duration, Premiere's eight colours, and the type (comment, chapter, segmentation, web link with its URL and frame
// target). Each change is one undo step (Markers.update); a field applies when it is accepted or loses focus.
Popover {
    id: root
    objectName: "markerEditor"

    property int markerIndex: -1
    readonly property var marker: markerIndex >= 0 && ProjectController.revision >= 0 ? Markers.marker(markerIndex) : ({})
    readonly property var types: ["comment", "chapter", "segmentation", "webLink"]
    readonly property var colorNames: [qsTr("Green"), qsTr("Red"), qsTr("Purple"), qsTr("Orange"), qsTr("Yellow"),
                                       qsTr("White"), qsTr("Blue"), qsTr("Cyan")]

    title: Markers.scope === "clip" ? qsTr("Clip marker") : qsTr("Marker")

    /// Opens for marker `index` (in Markers' current scope) at `point` in `item`.
    function openFor(index, item, point) {
        markerIndex = index
        const p = item.mapToItem(parent, point.x, point.y)
        x = Math.max(0, Math.min(parent.width - width, p.x - width / 2))
        y = Math.max(0, p.y + Theme.space(2))
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }

    function apply(fields) {
        if (root.markerIndex >= 0)
            Markers.update(root.markerIndex, fields)
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(76)
        spacing: Theme.space(2)

        FieldInput {
            id: nameField
            objectName: "markerNameField"
            width: parent.width
            placeholderText: qsTr("Name")
            text: root.marker.name ?? ""
            Accessible.name: qsTr("Marker name")
            onEditingFinished: {
                if (text !== (root.marker.name ?? ""))
                    root.apply({ name: text })
            }
        }
        FieldInput {
            objectName: "markerCommentField"
            width: parent.width
            placeholderText: qsTr("Comment")
            text: root.marker.comment ?? ""
            Accessible.name: qsTr("Marker comment")
            onEditingFinished: {
                if (text !== (root.marker.comment ?? ""))
                    root.apply({ comment: text })
            }
        }
        Row {
            spacing: Theme.space(2)
            Column {
                spacing: Theme.space(1)
                GroupLabel { text: qsTr("In") }
                FieldInput {
                    objectName: "markerInField"
                    width: Theme.space(34)
                    font.family: Theme.fontMono
                    text: root.marker.inText ?? ""
                    Accessible.name: qsTr("Marker In")
                    onEditingFinished: {
                        if (text !== (root.marker.inText ?? ""))
                            root.apply({ start: text })
                    }
                }
            }
            Column {
                spacing: Theme.space(1)
                GroupLabel { text: qsTr("Duration") }
                FieldInput {
                    objectName: "markerDurationField"
                    width: Theme.space(34)
                    font.family: Theme.fontMono
                    text: root.marker.durationText ?? ""
                    Accessible.name: qsTr("Marker duration")
                    onEditingFinished: {
                        if (text !== (root.marker.durationText ?? ""))
                            root.apply({ duration: text })
                    }
                }
            }
        }

        GroupLabel { text: qsTr("Colour: %1").arg(root.colorNames[((root.marker.colorIndex ?? 0) % 8 + 8) % 8]) }
        Row {
            spacing: Theme.space(1.5)
            Repeater {
                model: 8
                delegate: Swatch {
                    required property int index
                    objectName: "markerColor_" + index
                    swatchColor: Theme.markerColor(index)
                    tooltip: root.colorNames[index]
                    checked: (root.marker.colorIndex ?? -1) === index
                    onClicked: {
                        checked = Qt.binding(() => (root.marker.colorIndex ?? -1) === index)
                        root.apply({ colorIndex: index })
                    }
                }
            }
        }

        GroupLabel { text: qsTr("Type") }
        PillTabs {
            objectName: "markerTypeTabs"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Comment") }, { label: qsTr("Chapter") }, { label: qsTr("Segmentation") },
                    { label: qsTr("Web link") }]
            currentIndex: Math.max(0, root.types.indexOf(root.marker.type ?? "comment"))
            onActivated: index => root.apply({ type: root.types[index] })
            Accessible.name: qsTr("Marker type")
        }
        FieldInput {
            objectName: "markerUrlField"
            visible: (root.marker.type ?? "") === "webLink"
            width: parent.width
            placeholderText: qsTr("https://…")
            text: root.marker.url ?? ""
            Accessible.name: qsTr("Web link URL")
            onEditingFinished: {
                if (text !== (root.marker.url ?? ""))
                    root.apply({ url: text })
            }
        }
        FieldInput {
            objectName: "markerTargetField"
            visible: (root.marker.type ?? "") === "webLink"
            width: parent.width
            placeholderText: qsTr("Frame target")
            text: root.marker.target ?? ""
            Accessible.name: qsTr("Web link frame target")
            onEditingFinished: {
                if (text !== (root.marker.target ?? ""))
                    root.apply({ target: text })
            }
        }

        Row {
            spacing: Theme.space(2)
            PillButton {
                objectName: "markerDeleteButton"
                compact: true
                iconName: "trash-2"
                text: qsTr("Delete")
                onClicked: {
                    Markers.remove(root.markerIndex)
                    root.close()
                }
            }
            PillButton {
                compact: true
                variant: "primary"
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
