pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes

// One-click import (design language v3): a drop zone (files dropped anywhere on the Home screen land here;
// `dragActive` lights it in the accent) and one tile per editor, each saying in one line what to export there
// (Home.importSources). A Premiere .prproj waits for an explicit choice of the experimental reader (`pending`);
// files Wabcut cannot read show what to export instead (`guidance`), import errors show in `error`.
HomeCard {
    id: card
    objectName: "homeImportCard"

    property bool dragActive: false
    /// A .prproj waiting for the experimental reader: { url, fileName }, or null.
    property var pending: null
    property string guidance: ""
    property string error: ""

    signal chooseRequested(var source)        // a source of Home.importSources, or null for any timeline
    signal experimentalAccepted()
    signal pendingCancelled()

    title: qsTr("One-click import")
    hue: "mint"

    // Drop zone
    AbstractButton {
        id: zone
        objectName: "homeDropZone"
        Layout.fillWidth: true
        implicitHeight: Theme.space(30)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.name: qsTr("Drop a timeline here, or choose a file")
        onClicked: card.chooseRequested(null)

        scale: card.dragActive ? Theme.liftScale : 1
        Behavior on scale { NumberAnimation { duration: Theme.durBounce; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSpring } }

        background: Rectangle {
            radius: Theme.radiusCard
            color: card.dragActive ? Theme.accentWash : zone.hovered ? Theme.section : Theme.sunken
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            Shape {
                anchors.fill: parent
                ShapePath {
                    strokeColor: card.dragActive ? Theme.accentStroke : Theme.borderStrong
                    strokeWidth: card.dragActive ? Theme.focusRingWidth : Theme.hairline * 1.5
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 3]
                    fillColor: "transparent"
                    PathRectangle {
                        x: Theme.hairline
                        y: Theme.hairline
                        width: zone.width - Theme.hairline * 2
                        height: zone.height - Theme.hairline * 2
                        radius: Theme.radiusCard
                    }
                }
            }
        }

        contentItem: ColumnLayout {
            spacing: Theme.space(1)

            Item { Layout.fillHeight: true }
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: Theme.iconXl + Theme.space(5)
                implicitHeight: implicitWidth
                radius: width / 2
                color: card.dragActive ? Theme.accentFill : Theme.accentWash
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                Icon {
                    anchors.centerIn: parent
                    name: "download"
                    size: Theme.iconXl
                    color: card.dragActive ? Theme.accentOnFill : Theme.accentStroke
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: card.dragActive ? qsTr("Drop to import") : qsTr("Drop a timeline here")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXl
                font.weight: Theme.weightTitle
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Final Cut Pro XML · FCPXML · OpenTimelineIO · EDL · Premiere .prproj (experimental) — or click to choose a file")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Item { Layout.fillHeight: true }
        }

        FocusRing {
            targetRadius: Theme.radiusCard
            visible: zone.visualFocus
        }

        HoverHandler {
            enabled: zone.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }

    // A .prproj: the experimental reader, by explicit choice only.
    Rectangle {
        objectName: "homeExperimentalPrompt"
        Layout.fillWidth: true
        visible: card.pending !== null
        implicitHeight: prompt.implicitHeight + Theme.space(6)
        radius: Theme.radiusCard
        color: Qt.alpha(Theme.warning, 0.10)
        border.width: Theme.hairline
        border.color: Qt.alpha(Theme.warning, 0.55)

        ColumnLayout {
            id: prompt
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Icon {
                    name: "triangle-alert"
                    size: Theme.iconSm
                    color: Theme.warning
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 · Premiere Pro project, experimental reader").arg(card.pending ? card.pending.fileName : "")
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Wabcut can read the sequences, clips and cuts of a .prproj directly, but the format is not documented, so some effects may not come across: check the import report. For the most faithful import, export Final Cut Pro XML from Premiere.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Item { Layout.fillWidth: true }
                PillButton {
                    objectName: "homeExperimentalCancel"
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: card.pendingCancelled()
                }
                PillButton {
                    objectName: "homeExperimentalImport"
                    compact: true
                    variant: "primary"
                    text: qsTr("Import with the experimental reader")
                    onClicked: card.experimentalAccepted()
                }
            }
        }
    }

    // What to export instead, or why the import failed.
    Rectangle {
        objectName: "homeImportMessage"
        Layout.fillWidth: true
        visible: card.pending === null && (card.guidance !== "" || card.error !== "")
        implicitHeight: message.implicitHeight + Theme.space(5)
        radius: Theme.radiusCard
        color: Qt.alpha(card.error !== "" ? Theme.danger : Theme.info, 0.10)
        border.width: Theme.hairline
        border.color: Qt.alpha(card.error !== "" ? Theme.danger : Theme.info, 0.5)

        RowLayout {
            id: message
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)
            Icon {
                Layout.alignment: Qt.AlignTop
                name: card.error !== "" ? "triangle-alert" : "book-open"
                size: Theme.iconSm
                color: card.error !== "" ? Theme.danger : Theme.info
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: card.error !== "" ? card.error : card.guidance
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            IconButton {
                Layout.alignment: Qt.AlignTop
                compact: true
                iconName: "x"
                iconSize: Theme.iconXs
                tooltip: qsTr("Dismiss")
                onClicked: {
                    card.guidance = ""
                    card.error = ""
                }
            }
        }
    }

    // One button per editor
    GridLayout {
        Layout.fillWidth: true
        columns: width >= Theme.space(125) ? 3 : 1
        columnSpacing: Theme.space(2)
        rowSpacing: Theme.space(2)

        Repeater {
            model: Home.importSources
            delegate: AbstractButton {
                id: source
                required property var modelData
                required property int index
                objectName: "homeImport_" + modelData.id
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                implicitHeight: sourceColumn.implicitHeight + topPadding + bottomPadding
                padding: Theme.space(3)
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.name: qsTr("Import from %1").arg(modelData.name)
                Accessible.description: modelData.steps
                onClicked: card.chooseRequested(modelData)

                background: Rectangle {
                    radius: Theme.radiusCard - Theme.space(0.5)
                    color: source.down || source.hovered ? Theme.hover : Theme.raised
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }

                contentItem: ColumnLayout {
                    id: sourceColumn
                    spacing: Theme.space(1)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        // A neutral monogram chip in one of the hues, not the editor's logo.
                        Rectangle {
                            implicitWidth: Theme.controlHeightSmall
                            implicitHeight: Theme.controlHeightSmall
                            radius: height / 2
                            color: Theme.hue(["blue", "pink", "lime"][source.index % 3])

                            Text {
                                anchors.centerIn: parent
                                text: source.modelData.name.split(" ").map(word => word.charAt(0)).join("").slice(0, 2)
                                color: Theme.clipInk
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                font.weight: Theme.weightTitle
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Import from %1").arg(source.modelData.name)
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightTitle
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: source.modelData.steps
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        Layout.fillWidth: true
                        text: source.modelData.formats
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }

                FocusRing {
                    targetRadius: Theme.radiusCard - Theme.space(0.5)
                    visible: source.visualFocus
                }


                HoverHandler {
                    enabled: source.enabled
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
    }
}
