pragma ComponentBehavior: Bound
import QtQuick

// Timecode ruler above the lanes, aligned with the TimelineView. The view picks the label interval (1, 2, 5,
// 10, 15, 30 s, 1, 5, 10, 30 min, 1 h — the smallest at least Theme.rulerLabelSpacing apart) and formats the
// labels; this item only lays out a fixed number of tick delegates (count depends on the width, so scrolling
// and zooming re-evaluate bindings and never recreate items). Sequence markers show as flags in Premiere's marker
// colours with a bar to their end when they have a duration (M5i: drag a flag to move it, Alt-drag to set its
// duration, double-click to edit it), the
// sequence In/Out marks as an accent band along the bottom edge (I/O/X keys) and the playhead as a round white knob.
// Press/drag scrubs the playhead. 0.2: the render bar along the top edge; with Show Audio Time Units the labels
// count audio samples. UX rework (stream T): each marker's name beside its flag (cut short before the next marker,
// hidden where there is no room) and, on hover over the flag or the name, a tooltip with its name, comment, time and
// length (TimelineView.markerItems). Only the flag (and a range marker's bar) takes presses: the name is drawn under the
// scrub area and under the playhead knob, so pressing or dragging beside a flag scrubs like anywhere else on the ruler.
Item {
    id: root

    objectName: "timelineRuler"

    required property TimelineView view

    clip: true
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Timeline ruler")

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderStrong
    }

    Repeater {
        model: Math.max(0, Math.ceil(root.width / Theme.rulerLabelSpacing) + 2)

        delegate: Item {
            id: tick

            required property int index

            x: Math.round(root.view.rulerFirstX + index * root.view.rulerStepPx)
            width: root.view.rulerStepPx
            height: root.height

            Rectangle {
                x: 0
                y: tick.height - height
                width: Theme.hairline
                height: tick.height / 2
                color: Theme.borderStrong
            }

            // Minor ticks (at most 5 between labels). Positioned without `parent`: delegates outlive their
            // parent briefly while the outer Repeater shrinks on resize.
            Repeater {
                model: 5

                delegate: Rectangle {
                    required property int index
                    x: Math.round((index + 1) * root.view.rulerStepPx / root.view.rulerSubdivisions)
                    y: tick.height - height
                    width: Theme.hairline
                    height: Theme.space(1.5)
                    color: Theme.borderStrong
                    visible: index < root.view.rulerSubdivisions - 1
                }
            }

            Text {
                x: Theme.space(1)
                y: Theme.space(0.5)
                text: root.view.rulerLabel(root.view.rulerFirstIndex + tick.index, root.view.rulerStepFrames)
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }

    // 0.2, the render bar (Premiere's red / yellow / green bar, render.status) along the ruler's top edge: red segments
    // must be rendered to play in real time, yellow ones may play in real time, green ones are rendered (Render In to
    // Out). TimelineView recomputes it after edits; plain segments draw nothing.
    Item {
        id: renderBar
        objectName: "renderBar"
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.renderBarHeight
        Accessible.role: Accessible.Indicator
        Accessible.name: qsTr("Render bar")

        Repeater {
            model: root.view.renderBarItems

            delegate: Rectangle {
                required property var modelData
                readonly property real x0: modelData.start * root.view.pixelsPerSecond - root.view.scrollX
                readonly property real x1: modelData.end * root.view.pixelsPerSecond - root.view.scrollX
                objectName: "renderBarSegment_" + modelData.status
                x: Math.round(Math.max(-1, x0))
                width: Math.max(Theme.hairline, Math.round(Math.min(root.width + 1, x1)) - x)
                height: renderBar.height
                visible: x1 > 0 && x0 < root.width
                color: modelData.status === "red" ? Theme.renderRed
                       : modelData.status === "rendered" ? Theme.renderGreen : Theme.renderYellow
            }
        }
    }

    // Sequence In/Out marks: a bar from In to Out (open-ended when only one mark is set).
    Rectangle {
        id: markedRange
        objectName: "rulerMarkedRange"
        readonly property bool hasIn: !isNaN(root.view.markInX)
        readonly property bool hasOut: !isNaN(root.view.markOutX)
        readonly property real x0: hasIn ? root.view.markInX : 0
        readonly property real x1: hasOut ? root.view.markOutX : root.width
        visible: (hasIn || hasOut) && x1 > 0 && x0 < root.width
        x: Math.round(Math.max(0, x0))
        width: Math.max(Theme.hairline, Math.round(Math.min(root.width, x1)) - x)
        y: root.height - height - Theme.hairline
        height: Theme.space(2)
        radius: Theme.space(1)
        color: Qt.alpha(Theme.accentFill, 0.4)
    }

    // The markers' name pills (each marker delegate below puts its pill here): under the playhead knob, which stays
    // visible over a name, and under the scrub area, which keeps the presses.
    Item {
        id: markerNameLayer
        anchors.fill: parent
    }

    // Playhead knob (design language v3): a round white knob with a dark ring, over the line into the lanes.
    Rectangle {
        objectName: "playheadCap"
        readonly property real px: root.view.playheadX
        x: Math.round(px - width / 2)
        y: root.height - height - Theme.hairline
        width: Theme.playheadHead
        height: Theme.playheadHead
        visible: px > -width && px < root.width + width
        radius: width / 2
        color: Theme.playhead
        border.width: Theme.space(0.75)
        border.color: Theme.canvas
    }

    MouseArea {
        id: scrubArea
        anchors.fill: parent
        preventStealing: true
        onPressed: mouse => root.view.seekToX(mouse.x)
        onPositionChanged: mouse => {
            if (pressed)
                root.view.seekToX(mouse.x)
        }
    }

    // Sequence markers (M5i), over the scrub area: a flag in the marker's colour, a bar to its end for a range marker.
    Repeater {
        model: root.view.markerItems

        delegate: Item {
            id: marker

            required property var modelData
            // While dragging: where the flag (or, Alt-dragging, the end) is.
            property real dragX: NaN
            property bool settingEnd: false
            readonly property real startX: !isNaN(dragX) && !settingEnd ? dragX : modelData.x
            readonly property real endX: !isNaN(dragX) && settingEnd ? Math.max(dragX, modelData.x)
                                       : modelData.endX + (startX - modelData.x)
            readonly property color color: Theme.markerColor(modelData.colorIndex)
            // The name label: from the flag's right edge to just before the next marker's flag.
            readonly property real labelX: Theme.markerFlagWidth / 2 + Theme.space(1)
            readonly property real labelRoom: Math.min(Theme.space(40), (isFinite(modelData.nextX) ? modelData.nextX : root.width + Theme.space(40))
                                                                        - modelData.x - labelX - Theme.markerFlagWidth / 2 - Theme.space(1))
            readonly property bool showName: (modelData.name ?? "") !== "" && labelRoom >= Theme.space(6) && isNaN(dragX)

            objectName: "rulerMarker_" + modelData.index
            x: Math.round(startX)
            y: 0
            width: Math.max(Theme.markerFlagWidth, endX - startX)
            height: root.height
            visible: endX + Theme.markerFlagWidth > 0 && startX - Theme.markerFlagWidth < root.width

            Rectangle {
                visible: marker.endX - marker.startX >= 1
                x: 0
                y: root.height - height - Theme.hairline
                width: marker.endX - marker.startX
                height: Theme.space(1)
                radius: height / 2
                color: Qt.alpha(marker.color, 0.55)
            }
            // The flag: a pill rounded on top, square at its foot (the reference's marker tab).
            Rectangle {
                objectName: "rulerMarkerFlag"
                x: -Math.round(width / 2)
                y: root.height - height - Theme.space(1.5)
                width: Theme.markerFlagWidth + 1
                height: Theme.markerFlagWidth + 5
                radius: width / 2
                color: marker.color

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: parent.height / 2
                    radius: Theme.hairline * 2
                    color: marker.color
                }
            }
            // The marker's name on a small pill (over the tick labels) beside the flag, drawn in the name layer (under
            // the playhead knob and the scrub area). Hovering it shows the tooltip; presses go through to the scrub area.
            Rectangle {
                id: nameTag
                objectName: "rulerMarkerName_" + marker.modelData.index
                parent: markerNameLayer
                visible: marker.showName && marker.visible
                x: marker.x + marker.labelX
                y: root.height - Theme.markerFlagWidth - 5 - Theme.space(1.5)
                width: Math.min(marker.labelRoom, nameText.implicitWidth + Theme.space(2))
                height: Theme.markerFlagWidth + 5
                radius: height / 2
                color: Theme.raised

                Text {
                    id: nameText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.space(1)
                    anchors.rightMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    text: marker.modelData.name ?? ""
                    elide: Text.ElideRight
                    color: flagArea.containsMouse || nameHover.containsMouse ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
                // Hover only (no buttons): a press on the name scrubs, as it does anywhere off the flag.
                MouseArea {
                    id: nameHover
                    objectName: "rulerMarkerNameArea"
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    hoverEnabled: true
                }
            }
            // The flag (and a range marker's bar) takes presses: drag moves the marker, Alt-drag sets its end, a click
            // puts the playhead on it, a double-click edits it.
            MouseArea {
                id: flagArea
                objectName: "rulerMarkerArea"
                x: -Theme.markerFlagWidth / 2
                width: Math.max(Theme.markerFlagWidth, marker.endX - marker.startX + Theme.markerFlagWidth / 2)
                height: parent.height
                preventStealing: true
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                property real pressX: 0
                onPressed: mouse => {
                    pressX = mapToItem(root, mouse.x, 0).x
                    marker.settingEnd = (mouse.modifiers & Qt.AltModifier) !== 0
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return // a hover move (hoverEnabled: the tooltip)
                    const x = mapToItem(root, mouse.x, 0).x
                    if (isNaN(marker.dragX) && Math.abs(x - pressX) < Theme.space(1))
                        return
                    marker.dragX = x
                }
                onReleased: {
                    if (!isNaN(marker.dragX)) {
                        if (marker.settingEnd)
                            Markers.setEnd(marker.modelData.index, root.view.timeKeyAt(marker.dragX))
                        else
                            Markers.moveTo(marker.modelData.index, root.view.timeKeyAt(marker.dragX))
                    } else {
                        root.view.seekToX(marker.modelData.x)
                    }
                    marker.dragX = NaN
                }
                onDoubleClicked: mouse => {
                    Markers.scope = "sequence"
                    markerEditor.openFor(marker.modelData.index, flagArea, Qt.point(mouse.x, flagArea.height))
                }
            }
            // Hover: the marker's name, comment, time and length.
            Loader {
                active: (flagArea.containsMouse || nameHover.containsMouse) && !flagArea.pressed && !scrubArea.pressed
                        && !markerEditor.opened
                sourceComponent: Tooltip {
                    objectName: "rulerMarkerTooltip"
                    parent: marker
                    visible: true
                    text: {
                        const m = marker.modelData
                        const lines = [(m.name ?? "") !== "" ? m.name : qsTr("Marker")]
                        if ((m.comment ?? "") !== "")
                            lines.push(m.comment)
                        lines.push((m.durationText ?? "") !== "" ? qsTr("%1 · %2 long").arg(m.timecode).arg(m.durationText)
                                                                 : m.timecode)
                        return lines.join("\n")
                    }
                }
            }
        }
    }

    MarkerEditor {
        id: markerEditor
        parent: root
    }
}
