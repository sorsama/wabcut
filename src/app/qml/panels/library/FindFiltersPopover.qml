pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Find filters (M5k, the Library's funnel button): narrow the Library (and Smart results) by type, label, analysis,
// file state, use and date. Any active filter lists matches from every bin. Filter chips are tags, so they are pills.
Popover {
    id: root
    objectName: "libraryFindFilters"
    title: qsTr("Find")
    transformOrigin: Item.TopLeft

    readonly property var current: Library.filters

    // Library.filters is replaced as a whole: copy it, change one filter.
    function set(key, value) {
        const next = Object.assign({}, Library.filters)
        next[key] = value
        Library.filters = next
    }

    component Chip: AbstractButton {
        id: chip
        property bool on: false
        checkable: false
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
        HoverHandler {
            enabled: chip.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }
    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(76)
        spacing: Theme.space(2)

        GroupLabel {
            text: qsTr("Type")
        }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Repeater {
                model: [
                    { key: "", label: qsTr("Any") },
                    { key: "video", label: qsTr("Video") },
                    { key: "audio", label: qsTr("Audio") },
                    { key: "still", label: qsTr("Stills") },
                    { key: "subclip", label: qsTr("Subclips") },
                    { key: "sequence", label: qsTr("Sequences") },
                    { key: "title", label: qsTr("Titles") },
                    { key: "bin", label: qsTr("Bins") }
                ]
                delegate: Chip {
                    required property var modelData
                    objectName: "libraryFilterType_" + (modelData.key === "" ? "any" : modelData.key)
                    text: modelData.label
                    on: root.current.type === modelData.key
                    onClicked: root.set("type", modelData.key)
                }
            }
        }

        GroupLabel {
            text: qsTr("Label")
        }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Chip {
                objectName: "libraryFilterLabel_any"
                text: qsTr("Any")
                on: root.current.label < 0
                onClicked: root.set("label", -1)
            }
            Repeater {
                model: Theme.clipColors.length
                delegate: Swatch {
                    required property int index
                    objectName: "libraryFilterLabel_" + index
                    swatchColor: Theme.clipColor(index)
                    checked: root.current.label === index
                    tooltip: qsTr("Label %1").arg(index + 1)
                    onClicked: root.set("label", root.current.label === index ? -1 : index)
                }
            }
        }

        GroupLabel {
            text: qsTr("Show only")
        }
        Flow {
            width: parent.width
            spacing: Theme.space(1)
            Repeater {
                model: [
                    { key: "transcribed", label: qsTr("Transcribed") },
                    { key: "offline", label: qsTr("Offline") },
                    { key: "proxy", label: qsTr("With proxy") },
                    { key: "generated", label: qsTr("AI-generated") },
                    { key: "unused", label: qsTr("Unused") },
                    { key: "duplicates", label: qsTr("Duplicates") }
                ]
                delegate: Chip {
                    required property var modelData
                    objectName: "libraryFilter_" + modelData.key
                    text: modelData.label
                    on: root.current[modelData.key] === true
                    onClicked: root.set(modelData.key, !on)
                }
            }
        }

        GroupLabel {
            text: qsTr("File date (YYYY-MM-DD)")
        }
        Row {
            spacing: Theme.space(2)
            FieldInput {
                objectName: "libraryFilterFrom"
                width: Theme.space(34)
                Accessible.name: qsTr("File date from (YYYY-MM-DD)")
                placeholderText: qsTr("From")
                text: root.current.from
                inputMethodHints: Qt.ImhDate
                onEditingFinished: root.set("from", text)
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
            FieldInput {
                objectName: "libraryFilterTo"
                width: Theme.space(34)
                Accessible.name: qsTr("File date to (YYYY-MM-DD)")
                placeholderText: qsTr("To")
                text: root.current.to
                inputMethodHints: Qt.ImhDate
                onEditingFinished: root.set("to", text)
                Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
            }
        }

        PillButton {
            objectName: "libraryClearFilters"
            variant: "ghost"
            iconName: "x"
            text: qsTr("Clear filters")
            enabled: Library.filterCount > 0
            onClicked: Library.clearFilters()
        }
    }
}
