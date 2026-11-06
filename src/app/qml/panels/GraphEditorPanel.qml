pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes

// Graph Editor (0.7, card 96; Window > Graph Editor, the spline button in Effect Controls' header; docs/effects.md): the
// keyframes of the clip Effect Controls shows, drawn as curves in a panel that docks as big as the user likes. Every
// animated parameter has a colour and a row in the legend (the eye shows or hides its curve, a click names it the active
// curve whose values label the axis, a double click shows only that one); the value graph is on top, the speed graph (the
// curve's rate of change, with the Bezier handles' speed and influence) below it when "Speed graph" is on.
//   - Keys: click selects (Shift adds, Ctrl toggles), a box selects many, dragging moves the selected keys in whole frames
//     and in value (Shift holds one axis), Bezier handles of the selected keys drag (Shift levels, Alt breaks the pair),
//     a double click on a curve adds a key, Delete removes the selected ones, the arrow keys nudge (Shift: ten).
//   - View: the wheel zooms time (Shift: value, Alt: pans), the middle button pans, Fit / Fit selection / Reset view.
//   - Type a Time or a Value for the selected keys; the interpolation pills and the Easing library shape the segments.
// It edits the clip's own keyframes through Effect Controls' model: one undo step per gesture, the key selection shared
// with the lanes, so the lanes and this panel are two views of one thing.
Panel {
    id: panel
    objectName: "graphEditorPanel"

    /// Shown in the workspace now (Main.qml binds it).
    property bool opened: false

    title: qsTr("Graph Editor")

    readonly property var controls: ProjectController.effectControls
    readonly property var graph: ProjectController.effectControls.graph
    readonly property var info: graph.info
    readonly property int selCount: info.selCount ?? 0
    readonly property bool hasCurves: graph.legend.length > 0

    function focusContent() {
        valuePane.forceActiveFocus(Qt.OtherFocusReason)
    }

    Binding {
        target: panel.graph
        property: "active"
        value: panel.opened
    }

    function curveColor(index: int): color {
        return valuePane.curveColor(index)
    }

    // The keys the panes take while one of them has the focus (the keymap would otherwise move the playhead, delete the
    // timeline's clips ...): Delete / Backspace, the arrows, Home, F, Esc and Ctrl+A. Typing in the Time and Value fields is
    // theirs, as ever.
    Keys.onShortcutOverride: event => {
        if (!panel.hasCurves || !(valuePane.activeFocus || speedPane.activeFocus))
            return
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        event.accepted = [Qt.Key_Delete, Qt.Key_Backspace, Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home,
                          Qt.Key_Escape].includes(event.key)
                         || (event.key === Qt.Key_F && !ctrl && (event.modifiers & ~Qt.ShiftModifier) === 0)
                         || (event.key === Qt.Key_A && ctrl)
    }
    // The keyboard (the value pane has the focus after a click in a pane): Delete removes the selected keys, F fits, the
    // arrows nudge, Ctrl+A selects every key of the shown curves.
    Keys.onPressed: event => {
        if (!panel.hasCurves)
            return
        const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
        switch (event.key) {
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            panel.controls.deleteSelectedKeys()
            break
        case Qt.Key_F:
            panel.graph.fit(true)
            break
        case Qt.Key_Home:
            panel.graph.resetView()
            break
        case Qt.Key_Left:
            panel.graph.nudge(-step, 0)
            break
        case Qt.Key_Right:
            panel.graph.nudge(step, 0)
            break
        case Qt.Key_Up:
            panel.graph.nudge(0, step)
            break
        case Qt.Key_Down:
            panel.graph.nudge(0, -step)
            break
        case Qt.Key_A:
            if (!(event.modifiers & Qt.ControlModifier))
                return
            panel.graph.boxSelect("value", 0, -1e9, 1, 1e9, 0)
            break
        case Qt.Key_Escape:
            panel.graph.clearSelection()
            break
        default:
            return
        }
        event.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Graph Editor")

        // ---- Tools ----
        Flow {
            id: tools
            objectName: "graphEditorTools"
            Layout.fillWidth: true
            spacing: Theme.space(1.5)
            enabled: panel.hasCurves

            IconButton {
                objectName: "graphFitSelection"
                compact: true
                iconName: "crosshair"
                tooltip: qsTr("Fit the selected keys (F), or every key when none is selected")
                onClicked: panel.graph.fit(true)
            }
            IconButton {
                objectName: "graphFitAll"
                compact: true
                iconName: "zoom-out"
                tooltip: qsTr("Fit every key of the shown curves")
                onClicked: panel.graph.fit(false)
            }
            IconButton {
                objectName: "graphResetView"
                compact: true
                iconName: "rotate-ccw"
                tooltip: qsTr("Show the whole clip (Home)")
                onClicked: panel.graph.resetView()
            }
            Toggle {
                objectName: "graphSpeedToggle"
                text: qsTr("Speed graph")
                checked: panel.graph.speedGraph
                onToggled: panel.graph.speedGraph = checked
            }
            Toggle {
                objectName: "graphSharedScaleToggle"
                text: qsTr("Shared scale")
                tooltip: qsTr("One value scale for every curve (they are each fitted to their own range otherwise)")
                checked: panel.graph.sharedScale
                onToggled: panel.graph.sharedScale = checked
            }
            Toggle {
                id: legendToggle
                objectName: "graphLegendToggle"
                text: qsTr("Legend")
                checked: true
            }
        }

        // ---- The selected keys: interpolation, easing, numeric entry ----
        Flow {
            id: keyTools
            objectName: "graphEditorKeyTools"
            Layout.fillWidth: true
            spacing: Theme.space(1.5)
            enabled: panel.hasCurves

            Repeater {
                model: [
                    { key: "linear", label: qsTr("Linear") },
                    { key: "hold", label: qsTr("Hold") },
                    { key: "bezier", label: qsTr("Bezier") },
                    { key: "easeIn", label: qsTr("Ease In") },
                    { key: "easeOut", label: qsTr("Ease Out") },
                    { key: "easeInOut", label: qsTr("Ease In and Out") }
                ]
                delegate: PillButton {
                    id: shapeButton
                    required property var modelData
                    objectName: "graphShape_" + modelData.key
                    compact: true
                    text: modelData.label
                    enabled: panel.selCount > 0
                    variant: panel.info.selShape === modelData.key ? "primary" : "secondary"
                    tooltip: qsTr("Set the interpolation of the selected keys")
                    onClicked: panel.controls.setKeyInterpolation(modelData.key)
                }
            }
            PillButton {
                id: easingButton
                objectName: "graphEasingButton"
                compact: true
                iconName: "spline"
                text: qsTr("Easing")
                enabled: panel.selCount > 0
                tooltip: qsTr("Shape the segments after the selected keys with an easing preset")
                onClicked: {
                    easingPopover.x = Math.max(0, Math.min(panel.width - easingPopover.width, easingButton.mapToItem(panel, 0, 0).x))
                    easingPopover.y = Math.min(panel.height - easingPopover.height, easingButton.mapToItem(panel, 0, easingButton.height).y + Theme.space(1))
                    easingPopover.open()
                }
            }
            // Numeric entry for the selected key (or keys): the time of the first, the value of the active curve's keys.
            Row {
                spacing: Theme.space(1.5)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Time")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                FieldInput {
                    id: timeField
                    objectName: "graphTimeField"
                    width: Theme.space(36)
                    enabled: panel.selCount > 0
                    font.family: Theme.fontMono
                    text: panel.info.selTimeText ?? ""
                    Accessible.name: qsTr("Time of the selected key")
                    onAccepted: {
                        if (!panel.graph.setSelectionTime(text))
                            text = Qt.binding(() => panel.info.selTimeText ?? "")
                        panel.focusContent()
                    }
                }
            }
            Row {
                spacing: Theme.space(1.5)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.info.selTitle ?? qsTr("Value")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, Theme.space(30))
                }
                NumberField {
                    id: valueField
                    objectName: "graphValueField"
                    width: Theme.space(26)
                    enabled: panel.selCount > 0
                    value: panel.info.selValue ?? 0
                    from: panel.info.selFrom ?? -1e9
                    to: panel.info.selTo ?? 1e9
                    stepSize: panel.info.selStep ?? 1
                    decimals: panel.info.selDecimals ?? 0
                    suffix: panel.info.selUnit ?? ""
                    Accessible.name: qsTr("Value of the selected keys")
                    onValueModified: v => panel.graph.setSelectionValue(v)
                }
            }
        }

        // ---- Body ----
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.space(2)
            visible: panel.hasCurves

            // The legend: a row per animated parameter.
            Rectangle {
                id: legend
                objectName: "graphLegend"
                visible: legendToggle.checked && panel.width > Theme.space(120)
                Layout.preferredWidth: Theme.space(54)
                Layout.fillHeight: true
                radius: Theme.radiusClip
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1.5)
                    spacing: Theme.space(1)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(1)
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Properties")
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Theme.weightLabel
                        }
                        PillButton {
                            objectName: "graphShowAll"
                            compact: true
                            variant: "ghost"
                            text: qsTr("All")
                            tooltip: qsTr("Show every curve")
                            onClicked: panel.graph.showAll()
                        }
                        PillButton {
                            objectName: "graphShowSelected"
                            compact: true
                            variant: "ghost"
                            text: qsTr("Selected")
                            enabled: panel.selCount > 0
                            tooltip: qsTr("Show only the curves that have a selected key")
                            onClicked: panel.graph.showSelected()
                        }
                    }
                    ListView {
                        id: legendList
                        objectName: "graphLegendList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 1
                        model: panel.graph.legend
                        ScrollBar.vertical: ThinScrollBar {}
                        Accessible.role: Accessible.List
                        Accessible.name: qsTr("Animated parameters")
                        delegate: Rectangle {
                            id: entry
                            required property var modelData
                            required property int index
                            objectName: "graphLegendRow_" + modelData.row
                            width: legendList.width
                            height: Theme.controlHeight + Theme.space(2)
                            radius: Theme.radiusControl
                            color: modelData.active ? Theme.hover : entryHover.hovered ? Theme.raised : "transparent"
                            Accessible.role: Accessible.ListItem
                            Accessible.name: modelData.title + (modelData.section !== "" ? ", " + modelData.section : "")
                                             + ", " + modelData.valueText
                            Accessible.selected: modelData.active
                            HoverHandler { id: entryHover }
                            TapHandler {
                                onTapped: panel.graph.setActiveRow(entry.modelData.row)
                                onDoubleTapped: panel.graph.isolate(entry.modelData.row)
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.space(1.5)
                                anchors.rightMargin: Theme.space(1.5)
                                spacing: Theme.space(1.5)
                                Rectangle {
                                    Layout.preferredWidth: Theme.space(3)
                                    Layout.preferredHeight: Theme.space(3)
                                    radius: width / 2
                                    color: panel.curveColor(entry.modelData.color)
                                    opacity: entry.modelData.visible ? 1 : 0.35
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        Layout.fillWidth: true
                                        text: entry.modelData.title
                                        elide: Text.ElideRight
                                        color: entry.modelData.visible ? Theme.textPrimary : Theme.textTertiary
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textMd
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: entry.modelData.section !== "" ? entry.modelData.section + "  ·  " + entry.modelData.valueText
                                                                              : entry.modelData.valueText
                                        elide: Text.ElideRight
                                        color: Theme.textTertiary
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                                IconButton {
                                    objectName: "graphCurveVisible_" + entry.modelData.row
                                    compact: true
                                    iconName: entry.modelData.visible ? "eye" : "eye-off"
                                    tooltip: entry.modelData.visible ? qsTr("Hide this curve") : qsTr("Show this curve")
                                    onClicked: panel.graph.setCurveVisible(entry.modelData.row, !entry.modelData.visible)
                                }
                            }
                        }
                    }
                }
            }

            // The panes.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.space(1)

                // The time ruler: click or drag to move the playhead.
                Item {
                    id: ruler
                    objectName: "graphRuler"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.space(6)
                    Repeater {
                        model: valuePane.timeTicks
                        delegate: Text {
                            required property real modelData
                            x: Math.round(valuePane.xToPx(modelData)) + 3
                            anchors.verticalCenter: parent.verticalCenter
                            text: panel.graph.timeText(modelData)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    Rectangle {
                        visible: panel.graph.playhead >= 0
                        x: Math.round(valuePane.xToPx(panel.graph.playhead)) - 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: parent.height - 4
                        radius: 1
                        color: Theme.accentStroke
                    }
                    MouseArea {
                        anchors.fill: parent
                        function seekTo(px: real) { panel.graph.seek(valuePane.pxToX(Math.max(0, Math.min(width, px)))) }
                        onPressed: mouse => seekTo(mouse.x)
                        onPositionChanged: mouse => {
                            if (pressed)
                                seekTo(mouse.x)
                        }
                        cursorShape: Qt.PointingHandCursor
                    }
                }

                GraphEditorPane {
                    id: valuePane
                    objectName: "graphValuePane"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: Theme.space(20)
                    graph: panel.graph
                    kind: "value"
                    focus: true
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Pane
                    Accessible.name: qsTr("Value graph")
                }
                GraphEditorPane {
                    id: speedPane
                    objectName: "graphSpeedPane"
                    visible: panel.graph.speedGraph
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(Theme.space(24), valuePane.height * 0.55)
                    Layout.maximumHeight: Theme.space(80)
                    graph: panel.graph
                    kind: "speed"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Pane
                    Accessible.name: qsTr("Speed graph")
                }
            }
        }

        // ---- Empty and status ----
        EmptyState {
            objectName: "graphEmptyState"
            visible: !panel.hasCurves
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            text: panel.info.hasClip === false && (panel.controls.mode !== "clip")
                  ? qsTr("Select a clip to edit its keyframes as curves.")
                  : qsTr("Nothing is animated yet. Turn on a parameter's stopwatch in Effect Controls and its curve appears here.")
        }
        Text {
            objectName: "graphStatus"
            Layout.fillWidth: true
            elide: Text.ElideRight
            visible: panel.hasCurves
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            text: (panel.selCount > 0 ? qsTr("%n key(s) selected", "", panel.selCount) + "  ·  " : "")
                  + (panel.info.clipTitle ?? "") + "  ·  "
                  + qsTr("Wheel: zoom time (Shift: value, Alt: pan).  Drag a key to move it, double-click a curve to add one.")
        }
    }

    // ---- Easing presets ----
    Popover {
        id: easingPopover
        objectName: "graphEasingPopover"
        title: qsTr("Easing")
        property var presets: []
        onAboutToShow: presets = panel.graph.easingPresets()

        Item {
            width: Math.min(panel.width - Theme.space(10), Theme.space(112))
            height: Math.min(panel.height - Theme.space(22), Theme.space(88))

            GridView {
                id: presetGrid
                objectName: "graphEasingGrid"
                anchors.fill: parent
                clip: true
                cellWidth: Theme.space(26)
                cellHeight: Theme.space(25)
                model: easingPopover.presets
                ScrollBar.vertical: ThinScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Easing presets")
                delegate: Item {
                    id: card
                    required property var modelData
                    width: presetGrid.cellWidth
                    height: presetGrid.cellHeight
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: Theme.space(1)
                        radius: Theme.radiusControl
                        color: cardHover.hovered ? Theme.hover : Theme.raised
                        border.width: Theme.hairline
                        border.color: Theme.border
                        Accessible.role: Accessible.Button
                        Accessible.name: card.modelData.name
                        Shape {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: Theme.space(2)
                            width: Theme.space(14)
                            height: Theme.space(11)
                            layer.enabled: true
                            layer.samples: 4
                            ShapePath {
                                strokeColor: Theme.accentStroke
                                strokeWidth: 2
                                fillColor: "transparent"
                                joinStyle: ShapePath.RoundJoin
                                PathPolyline {
                                    path: {
                                        const t = card.modelData.thumb
                                        const w = Theme.space(14)
                                        const h = Theme.space(11)
                                        const pts = []
                                        // The thumbnail's y runs 0..1 upwards (overshoots leave that), drawn inside a margin.
                                        for (let i = 0; i + 1 < t.length; i += 2)
                                            pts.push(Qt.point(t[i] * w, h * 0.85 - t[i + 1] * h * 0.7))
                                        return pts
                                    }
                                }
                            }
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Theme.space(1)
                            horizontalAlignment: Text.AlignHCenter
                            text: card.modelData.name
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                        HoverHandler { id: cardHover }
                        TapHandler {
                            onTapped: {
                                panel.graph.ease(card.modelData.id)
                                easingPopover.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
