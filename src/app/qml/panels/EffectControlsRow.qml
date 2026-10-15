pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Shapes

// One row of Effect Controls (ProjectController.effectControls): a section header (collapse chevron, drag grip, fx
// switch, reset, delete), a parameter (stopwatch, slider + number, reset, keyframe navigation; its keys drawn in
// the lane on the right), a switch, a timecode field or a choice (transition rows). Values are UI units; every
// change goes back through EffectControls (one undo step per drag, merged until endEdit()).
// An animated parameter's lane can open into its value graph (the graph button): the curve, the keys at their values
// and, on Bezier keys, in/out handles (InspectorPanel's lane area drags them: Alt breaks the pair).
Item {
    id: row

    // ---- Model roles ----
    required property int index
    required property string rowKey
    required property string kind
    required property string section
    required property string sectionKind
    required property string title
    required property string icon
    required property string unit
    required property real from
    required property real to
    required property real step
    required property int decimals
    required property real origin
    required property real value
    required property real defaultValue
    required property bool animatable
    required property bool animated
    required property bool keyAtPlayhead
    required property bool hasPreviousKey
    required property bool hasNextKey
    required property var keys
    required property bool effectEnabled
    required property bool collapsed
    required property bool removable
    required property bool movable
    required property bool resettable
    required property int stackIndex
    required property string stackKind
    required property string text
    required property var options
    required property string description
    required property bool known
    required property bool graphOpen
    required property var graph

    // ---- From the panel ----
    /// Width of the keyframe lane on the right (0: no lane; the slider gets the room).
    property real laneWidth: 0
    readonly property bool hasLane: laneWidth > 0
    readonly property var controls: ProjectController.effectControls
    readonly property bool header: kind === "header"
    /// M5m (A.6): an audio plugin's header shows its badge and "Edit plugin…" (Plugins.effectInfo; `revision` re-reads it).
    readonly property var pluginInfo: header && sectionKind === "effect" && Plugins.revision >= 0 ? Plugins.effectInfo(section) : ({})
    readonly property bool isPlugin: pluginInfo.plugin === true
    readonly property bool showGraph: graphOpen && !header && hasLane
    /// The value graph's area (lane coordinates for handle hits).
    readonly property alias graphItem: graphArea
    // The value shown: follows `value`; user edits update it at once (like PropSlider).
    property real shownValue: value
    onValueChanged: shownValue = value
    readonly property bool modified: Math.abs(shownValue - defaultValue) > Math.pow(10, -decimals) / 2

    // Header reorder: the dragged header lifts and follows the pointer; rows animate to their new place.
    property real dragOffset: 0
    readonly property bool dragging: grip.active

    function setFromUser(v) {
        const q = Math.pow(10, decimals)
        const next = Math.min(Math.max(from, to), Math.max(Math.min(from, to), Math.round(v * q) / q))
        if (next === shownValue)
            return
        shownValue = next
        controls.setValue(rowKey, next)
    }

    // A Bezier handle of the value graph: the line from its key, and the knob at its end.
    component HandleLine: Rectangle {
        property real fromX: 0
        property real fromY: 0
        property real toX: 0
        property real toY: 0
        x: fromX
        y: fromY - height / 2
        width: Math.hypot(toX - fromX, toY - fromY)
        height: Theme.hairline
        transformOrigin: Item.Left
        rotation: Math.atan2(toY - fromY, toX - fromX) * 180 / Math.PI
    }
    component HandleKnob: Rectangle {
        property real atX: 0
        property real atY: 0
        x: Math.round(atX - width / 2)
        y: Math.round(atY - height / 2)
        width: Theme.space(2) + Theme.hairline * 2
        height: width
        radius: width / 2
        color: Theme.raised
        border.width: Theme.focusRingWidth
    }

    width: ListView.view ? ListView.view.width : 0
    height: header ? Theme.controlHeight + Theme.space(1) : showGraph ? Theme.controlHeight + Theme.space(24) : Theme.controlHeight
    z: dragging ? 10 : 0

    Item {
        id: content
        width: row.width
        height: row.height
        y: row.dragOffset
        scale: row.dragging ? Theme.liftScale : 1
        Behavior on scale { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }

        // ---- Lane (right): the section band or the parameter's keys ----
        Rectangle {
            id: lane
            visible: row.hasLane
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: row.header ? Theme.space(2) : 1
            anchors.bottomMargin: row.header ? Theme.space(2) : 1
            width: row.laneWidth
            radius: row.header ? Theme.radiusClip : 0
            color: row.header ? (row.sectionKind === "transition" ? Theme.accentWash : Theme.raised) : "transparent"
            opacity: row.effectEnabled ? 1 : Theme.disabledOpacity

            Rectangle {
                visible: !row.header && !row.showGraph
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: Theme.hairline
                color: Theme.border
            }

            Repeater {
                model: row.header || row.showGraph ? [] : row.keys
                delegate: KeyframeShape {
                    required property var modelData
                    x: Math.round(modelData.x * lane.width - width / 2)
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    shape: modelData.shape
                    selected: modelData.selected
                }
            }

            // Value graph: the curve, its keys at their values and the Bezier handles.
            Item {
                id: graphArea
                objectName: "valueGraph"
                visible: row.showGraph
                anchors.fill: parent
                anchors.topMargin: Theme.space(2)
                anchors.bottomMargin: Theme.space(2)

                readonly property var keyPoints: row.graph && row.graph.keys ? row.graph.keys : []

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusClip
                    color: Theme.sunken
                    border.width: Theme.hairline
                    border.color: Theme.border
                }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: Theme.accentStroke
                        strokeWidth: Theme.focusRingWidth
                        fillColor: "transparent"
                        joinStyle: ShapePath.RoundJoin
                        PathPolyline {
                            path: {
                                const flat = row.graph && row.graph.path ? row.graph.path : []
                                const points = []
                                for (let i = 0; i + 1 < flat.length; i += 2)
                                    points.push(Qt.point(flat[i] * graphArea.width, flat[i + 1] * graphArea.height))
                                return points
                            }
                        }
                    }
                }
                Repeater {
                    model: graphArea.keyPoints
                    delegate: Item {
                        id: point
                        required property var modelData
                        anchors.fill: parent
                        readonly property real kx: modelData.x * graphArea.width
                        readonly property real ky: modelData.y * graphArea.height
                        readonly property color handleColor: modelData.broken ? Theme.warning : Theme.accentStroke

                        HandleLine {
                            visible: point.modelData.hasIn
                            fromX: point.kx
                            fromY: point.ky
                            toX: point.modelData.inX * graphArea.width
                            toY: point.modelData.inY * graphArea.height
                            color: point.handleColor
                        }
                        HandleLine {
                            visible: point.modelData.hasOut
                            fromX: point.kx
                            fromY: point.ky
                            toX: point.modelData.outX * graphArea.width
                            toY: point.modelData.outY * graphArea.height
                            color: point.handleColor
                        }
                        HandleKnob {
                            objectName: "handleIn"
                            visible: point.modelData.hasIn
                            atX: point.modelData.inX * graphArea.width
                            atY: point.modelData.inY * graphArea.height
                            border.color: point.handleColor
                        }
                        HandleKnob {
                            objectName: "handleOut"
                            visible: point.modelData.hasOut
                            atX: point.modelData.outX * graphArea.width
                            atY: point.modelData.outY * graphArea.height
                            border.color: point.handleColor
                        }
                        KeyframeShape {
                            x: Math.round(point.kx - width / 2)
                            y: Math.round(point.ky - height / 2)
                            shape: point.modelData.shape
                            selected: point.modelData.selected
                        }
                    }
                }
            }
        }

        // ---- Header ----
        Rectangle {
            visible: row.header
            anchors.left: parent.left
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: row.hasLane ? Theme.space(2) : 0
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.controlHeight - Theme.space(1)
            radius: Theme.radiusField
            color: row.dragging ? Theme.hover : Theme.raised
            border.width: Theme.hairline
            border.color: row.dragging ? Theme.accentStroke : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(1)
                anchors.rightMargin: Theme.space(1)
                spacing: Theme.space(1)

                IconButton {
                    objectName: "sectionChevron"
                    compact: true
                    iconName: "chevron-down"
                    iconSize: Theme.iconSm
                    rotation: row.collapsed ? -90 : 0
                    tooltip: row.collapsed ? qsTr("Expand %1").arg(row.title) : qsTr("Collapse %1").arg(row.title)
                    onClicked: row.controls.setCollapsed(row.section, !row.collapsed)
                    Behavior on rotation { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }
                }

                Icon {
                    id: gripIcon
                    visible: row.movable
                    name: "grip-vertical"
                    size: Theme.iconSm
                    color: grip.active || grip.containsMouse ? Theme.textSecondary : Theme.textTertiary

                    // Drag to reorder (a MouseArea: it keeps the press from the list, so the list does not scroll).
                    MouseArea {
                        id: grip
                        objectName: "sectionGrip"
                        anchors.fill: parent
                        anchors.margins: -Theme.space(1)
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor

                        property real pressY: 0
                        property bool active: false

                        onPressed: mouse => {
                            pressY = mapToItem(null, mouse.x, mouse.y).y
                            active = false
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return
                            const dy = mapToItem(null, mouse.x, mouse.y).y - pressY
                            if (!active && Math.abs(dy) < Application.styleHints.startDragDistance)
                                return
                            active = true
                            row.dragOffset = dy
                        }
                        onReleased: {
                            if (active) {
                                const view = row.ListView.view
                                if (view) {
                                    const target = view.indexAt(Theme.space(2), row.y + row.dragOffset + row.height / 2)
                                    const stackIndex = row.controls.stackIndexForRow(target < 0 ? (row.dragOffset < 0 ? 0 : view.count - 1) : target,
                                                                                     row.stackKind)
                                    row.controls.moveEffect(row.section, stackIndex)
                                }
                            }
                            active = false
                            row.dragOffset = 0
                        }
                        onCanceled: {
                            active = false
                            row.dragOffset = 0
                        }
                    }
                }

                IconButton {
                    objectName: "effectEnabledButton"
                    visible: row.sectionKind === "effect"
                    compact: true
                    checkable: true
                    checked: row.effectEnabled
                    iconName: "power"
                    iconSize: Theme.iconSm
                    tooltip: row.effectEnabled ? qsTr("Turn %1 off").arg(row.title) : qsTr("Turn %1 on").arg(row.title)
                    onClicked: row.controls.setEffectEnabled(row.section, !row.effectEnabled)
                }

                Icon {
                    name: row.icon
                    size: Theme.iconSm
                    color: row.sectionKind === "intrinsic" ? Theme.textTertiary : Theme.accentStroke
                    visible: row.icon !== ""
                }

                Text {
                    Layout.fillWidth: true
                    text: row.known ? row.title : qsTr("%1 (not available)").arg(row.title)
                    elide: Text.ElideRight
                    color: row.effectEnabled ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    font.strikeout: !row.effectEnabled
                }

                PluginBadge {
                    kind: row.pluginInfo.kind ?? ""
                    detail: row.pluginInfo.text ?? ""
                    missedBlocks: row.pluginInfo.missedBlocks ?? 0
                }

                IconButton {
                    objectName: "editPluginButton"
                    visible: row.isPlugin && row.pluginInfo.kind !== "missing"
                    enabled: row.pluginInfo.hasEditor === true
                    compact: true
                    iconName: "external-link"
                    iconSize: Theme.iconSm
                    tooltip: row.pluginInfo.hasEditor === true ? qsTr("Edit plugin… (%1)").arg(row.pluginInfo.name ?? row.title)
                                                               : qsTr("%1 has no editor window").arg(row.pluginInfo.name ?? row.title)
                    onClicked: Plugins.openEditor(row.section) // an error shows as Plugins.message
                }

                IconButton {
                    objectName: "sectionReset"
                    visible: row.resettable
                    compact: true
                    iconName: "rotate-ccw"
                    iconSize: Theme.iconSm
                    tooltip: qsTr("Reset %1").arg(row.title)
                    onClicked: row.controls.resetSection(row.section)
                }

                IconButton {
                    objectName: "sectionRemove"
                    visible: row.removable
                    compact: true
                    iconName: "trash-2"
                    iconSize: Theme.iconSm
                    tooltip: qsTr("Remove %1").arg(row.title)
                    onClicked: row.controls.removeSection(row.section)
                }
            }
        }

        // ---- Parameter / switch / timecode / choice ----
        RowLayout {
            visible: !row.header
            anchors.left: parent.left
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: row.hasLane ? Theme.space(2) : 0
            anchors.top: parent.top
            height: Theme.controlHeight
            anchors.leftMargin: Theme.space(1)
            spacing: Theme.space(1)
            opacity: row.effectEnabled ? 1 : Theme.disabledOpacity

            // Stopwatch: animate this parameter (keyframes).
            IconButton {
                objectName: "stopwatch"
                Layout.preferredWidth: implicitWidth
                visible: row.kind === "param" && row.animatable
                compact: true
                checkable: true
                checked: row.animated
                iconName: "timer"
                iconSize: Theme.iconSm
                tooltip: row.animated ? qsTr("Stop animating %1 (deletes its keyframes)").arg(row.title)
                                      : qsTr("Animate %1").arg(row.title)
                onClicked: row.controls.setAnimated(row.rowKey, !row.animated)
            }
            Item {
                visible: !(row.kind === "param" && row.animatable)
                Layout.preferredWidth: Theme.space(7)
                Layout.preferredHeight: 1
            }

            Text {
                Layout.preferredWidth: Theme.propLabelWidth
                text: row.title
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }

            ValueSlider {
                visible: row.kind === "param" && !row.hasLane
                Layout.fillWidth: true
                Layout.minimumWidth: Theme.space(10)
                from: row.from
                to: row.to
                origin: row.origin
                value: row.shownValue
                stepSize: row.step
                onMoved: row.setFromUser(value)
                onPressedChanged: {
                    if (!pressed)
                        row.controls.endEdit()
                }
                Accessible.name: row.title
            }
            Item {
                visible: row.kind !== "param" || row.hasLane
                Layout.fillWidth: true
                Layout.preferredHeight: 1
            }

            NumberField {
                visible: row.kind === "param"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: Math.min(row.from, row.to)
                to: Math.max(row.from, row.to)
                value: row.shownValue
                stepSize: row.step
                dragStep: (row.to - row.from) / 400
                decimals: row.decimals
                suffix: row.unit
                onValueModified: v => row.setFromUser(v)
                onEditingFinished: row.controls.endEdit()
                onScrubFinished: row.controls.endEdit()
                Accessible.name: row.title
            }

            Toggle {
                objectName: "paramToggle"
                visible: row.kind === "toggle"
                checked: row.value > (row.from + row.to) / 2
                tooltip: row.title
                onToggled: {
                    row.controls.setValue(row.rowKey, checked ? row.to : row.from)
                    row.controls.endEdit()
                }
            }

            TextField {
                id: timecodeField
                objectName: "timecodeField"
                visible: row.kind === "timecode"
                Layout.preferredWidth: Theme.numberFieldWidth + Theme.space(6)
                implicitHeight: Theme.controlHeightSmall
                text: row.text
                horizontalAlignment: TextInput.AlignRight
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: Theme.space(3.5)
                rightPadding: Theme.space(3.5)
                selectByMouse: true
                font.family: Theme.fontMono
                font.pixelSize: Theme.textMd
                color: Theme.textPrimary
                selectionColor: Theme.accentFill
                selectedTextColor: Theme.accentOnFill
                Accessible.name: row.title
                onEditingFinished: {
                    if (text !== row.text)
                        row.controls.setTransitionDuration(text)
                    text = Qt.binding(() => row.text)
                }
                background: Rectangle {
                    radius: height / 2
                    color: Theme.raised
                    border.width: timecodeField.activeFocus ? Theme.focusRingWidth : 0
                    border.color: Theme.focus
                }
            }

            Flow {
                visible: row.kind === "choice"
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.space(70)
                spacing: Theme.space(1)
                layoutDirection: Qt.RightToLeft

                Repeater {
                    model: row.kind === "choice" ? row.options : []
                    delegate: PillButton {
                        required property var modelData
                        compact: true
                        text: modelData.label
                        variant: row.text === modelData.value ? "primary" : "secondary"
                        enabled: modelData.value !== "custom"
                        Accessible.checkable: true
                        Accessible.checked: row.text === modelData.value
                        onClicked: {
                            if (row.rowKey === "t:alignment")
                                row.controls.setTransitionAlignment(modelData.value)
                            else
                                row.controls.setTransitionEffect(modelData.value)
                        }
                    }
                }
            }

            IconButton {
                objectName: "paramReset"
                visible: row.kind === "param"
                enabled: row.modified
                opacity: row.modified ? 1 : 0
                compact: true
                iconName: "rotate-ccw"
                iconSize: Theme.iconSm
                tooltip: qsTr("Reset %1").arg(row.title)
                onClicked: row.controls.resetValue(row.rowKey)
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            }

            // Keyframe navigation: previous key, add/remove the key at the playhead, next key.
            Row {
                visible: row.kind === "param" && row.animated
                spacing: 0

                IconButton {
                    objectName: "previousKey"
                    compact: true
                    iconName: "chevron-left"
                    iconSize: Theme.iconXs
                    enabled: row.hasPreviousKey
                    tooltip: qsTr("Previous keyframe of %1").arg(row.title)
                    onClicked: row.controls.goToKey(row.rowKey, -1)
                }
                IconButton {
                    objectName: "toggleKey"
                    compact: true
                    checkable: true
                    checked: row.keyAtPlayhead
                    iconName: "diamond"
                    iconSize: Theme.iconXs
                    tooltip: row.keyAtPlayhead ? qsTr("Remove the keyframe of %1 at the playhead").arg(row.title)
                                               : qsTr("Add a keyframe of %1 at the playhead").arg(row.title)
                    onClicked: row.controls.toggleKeyAtPlayhead(row.rowKey)
                }
                IconButton {
                    objectName: "nextKey"
                    compact: true
                    iconName: "chevron-right"
                    iconSize: Theme.iconXs
                    enabled: row.hasNextKey
                    tooltip: qsTr("Next keyframe of %1").arg(row.title)
                    onClicked: row.controls.goToKey(row.rowKey, 1)
                }
                IconButton {
                    objectName: "graphToggle"
                    visible: row.hasLane
                    compact: true
                    checkable: true
                    checked: row.graphOpen
                    iconName: "spline"
                    iconSize: Theme.iconXs
                    tooltip: row.graphOpen ? qsTr("Hide the value graph of %1").arg(row.title)
                                           : qsTr("Show the value graph of %1 (Bezier handles)").arg(row.title)
                    onClicked: row.controls.setGraphOpen(row.rowKey, !row.graphOpen)
                }
            }
        }
    }

    // Springs back into the list after a reorder drag (the model move animates the other rows).
    Behavior on dragOffset {
        enabled: !row.dragging && !Theme.reduceMotion
        SpringAnimation {
            spring: Theme.springStiffness
            damping: Theme.springDamping
            mass: Theme.springMass
            epsilon: Theme.springEpsilon
        }
    }
}
