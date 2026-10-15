pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// ● MARKERS (M5i, Markers): the sequence's markers or the selected clip's (clip markers live on its media and show on
// every clip of it), filtered by Premiere's eight colours and a search over names and comments. Click (or Enter) a row
// to move the playhead there, double-click to edit it in place, Delete removes it. The header adds a marker at the
// playhead and exports the sequence's markers as CSV or YouTube chapters.
Panel {
    id: root

    objectName: "markersPanel"
    title: qsTr("Markers")

    readonly property var colorNames: [qsTr("Green"), qsTr("Red"), qsTr("Purple"), qsTr("Orange"), qsTr("Yellow"),
                                       qsTr("White"), qsTr("Blue"), qsTr("Cyan")]
    readonly property var typeLabels: ({ chapter: qsTr("Chapter"), segmentation: qsTr("Segmentation"),
                                          webLink: qsTr("Web link"), comment: "" })

    // Focus target for the rail's "Markers" shortcut.
    function focusContent() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    leading: [
        IconButton {
            objectName: "markersAddButton"
            compact: true
            iconName: "bookmark"
            tooltip: qsTr("Add marker at the playhead")
            command: "marker.add"
            onClicked: {
                if (Markers.scope === "clip" && ProjectController.selectedClips.length > 0)
                    Markers.addClipMarker(ProjectController.selectedClips[0])
                else
                    Markers.add(false)
            }
        },
        IconButton {
            objectName: "markersExportButton"
            compact: true
            iconName: "file-down"
            tooltip: qsTr("Export the sequence's markers")
            onClicked: exportMenu.popup()
        }
    ]

    MarkersExportMenu {
        id: exportMenu
        onExportRequested: format => {
            exportDialog.format = format
            exportDialog.nameFilters = format === "csv" ? [qsTr("CSV files (*.csv)")] : [qsTr("Text files (*.txt)")]
            exportDialog.defaultSuffix = format === "csv" ? "csv" : "txt"
            exportDialog.open()
        }
    }

    FileDialog {
        id: exportDialog
        property string format: "csv"
        title: qsTr("Export markers")
        fileMode: FileDialog.SaveFile
        onAccepted: Markers.exportToFile(format, selectedFile)
    }

    Column {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Theme.space(2)

        PillTabs {
            objectName: "markersScopeTabs"
            width: parent.width
            tabs: true
            model: [{ label: qsTr("Sequence") }, { label: qsTr("Clip") }]
            currentIndex: Markers.scope === "clip" ? 1 : 0
            onActivated: index => Markers.scope = index === 1 ? "clip" : "sequence"
            Accessible.name: qsTr("Markers of")
        }
        FieldInput {
            id: search
            objectName: "markersSearch"
            width: parent.width
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search markers")
            text: Markers.search
            onTextChanged: Markers.search = text
            Accessible.name: qsTr("Search markers")
        }
        Row {
            spacing: Theme.space(1)
            Repeater {
                model: 8
                delegate: Swatch {
                    required property int index
                    objectName: "markerFilter_" + index
                    swatchColor: Theme.markerColor(index)
                    tooltip: qsTr("Show %1 markers").arg(root.colorNames[index])
                    checked: (Markers.colorMask & (1 << index)) !== 0
                    onClicked: {
                        checked = Qt.binding(() => (Markers.colorMask & (1 << index)) !== 0)
                        Markers.colorMask = Markers.colorMask ^ (1 << index)
                    }
                }
            }
        }
        Text {
            visible: Markers.scope === "clip"
            text: Markers.clipName !== "" ? qsTr("Clip: %1").arg(Markers.clipName) : qsTr("Select a clip to see its markers")
            color: Theme.textTertiary
            elide: Text.ElideRight
            width: parent.width
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    ListView {
        id: list
        objectName: "markersList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: controls.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.space(2)
        clip: true
        spacing: Theme.space(1)
        model: Markers.items
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: AbstractButton {
            id: row

            required property var modelData
            required property int index

            objectName: "markerRow_" + modelData.index
            width: list.width
            height: Theme.controlHeight + Theme.space(4)
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            onClicked: Markers.seek(modelData.index)
            onDoubleClicked: editor.openFor(modelData.index, row, Qt.point(row.width / 2, row.height))
            Keys.onDeletePressed: Markers.remove(modelData.index)
            Keys.onReturnPressed: Markers.seek(modelData.index)
            Accessible.role: Accessible.ListItem
            Accessible.name: qsTr("Marker %1 at %2").arg(modelData.name).arg(modelData.inText)

            contentItem: Item {
                Rectangle {
                    id: chip
                    x: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space(1)
                    height: parent.height - Theme.space(2)
                    radius: Theme.radiusBadge / 2
                    color: Theme.markerColor(row.modelData.colorIndex)
                }
                Column {
                    anchors.left: chip.right
                    anchors.leftMargin: Theme.space(3)
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space(0.5)
                    Row {
                        width: parent.width
                        spacing: Theme.space(2)
                        Text {
                            width: Math.min(implicitWidth, parent.width - typeBadge.width - Theme.space(2))
                            text: row.modelData.name !== "" ? row.modelData.name : qsTr("Marker")
                            elide: Text.ElideRight
                            color: row.modelData.name !== "" ? Theme.textPrimary : Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        Rectangle {
                            id: typeBadge
                            anchors.verticalCenter: parent.verticalCenter
                            visible: (root.typeLabels[row.modelData.type] ?? "") !== ""
                            width: visible ? typeText.implicitWidth + Theme.space(2) : 0
                            height: typeText.implicitHeight + Theme.space(0.5)
                            radius: Theme.radiusBadge
                            color: Theme.sunken
                            border.width: Theme.hairline
                            border.color: Theme.border
                            Text {
                                id: typeText
                                anchors.centerIn: parent
                                text: root.typeLabels[row.modelData.type] ?? ""
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: Theme.space(2)
                        Text {
                            id: times
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.ranged ? qsTr("%1 · %2").arg(row.modelData.inText).arg(row.modelData.durationText)
                                                       : row.modelData.inText
                            color: row.modelData.atPlayhead ? Theme.accentStroke : Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: text !== ""
                            width: parent.width - times.width - Theme.space(2)
                            text: row.modelData.type === "webLink" && row.modelData.url !== "" ? row.modelData.url : row.modelData.comment
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                }
            }

            background: Rectangle {
                radius: Theme.radiusField
                color: row.down ? Theme.raised : row.hovered ? Theme.hover : row.modelData.atPlayhead ? Theme.sunken : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                FocusRing {
                    anchors.margins: 0
                    targetRadius: Theme.radiusField
                    visible: row.visualFocus
                }
            }

            HoverHandler {
                enabled: row.enabled
                cursorShape: Qt.PointingHandCursor
            }
        }
    }

    EmptyState {
        objectName: "markersEmptyState"
        anchors.centerIn: list
        width: list.width - Theme.space(8)
        visible: list.count === 0
        illustration: Markers.totalCount > 0 ? "search" : "markers"
        //: %1 = the Add Marker shortcut
        text: Markers.totalCount > 0 ? qsTr("No markers match the search or the colours")
                                     : qsTr("No markers yet. Press %1 to add one at the playhead.")
                                           .arg((KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText("marker.add") : "") || "M")
    }

    MarkerEditor {
        id: editor
        parent: root
    }
}
