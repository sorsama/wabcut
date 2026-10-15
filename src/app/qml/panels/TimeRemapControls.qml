pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

ColumnLayout {
    id: root
    objectName: "timeRemapControls"
    property bool expanded: false
    property bool speedView: false
    // The key the graph's keys act on (an index into the curve's keys), or -1. It belongs to one clip's curve: it is
    // cleared when another clip is shown and after its key is deleted, and follows a key inserted at the playhead
    // (review P2-44).
    property int selectedKey: -1
    readonly property string shownClip: ProjectController.selectedClip.id ?? ""
    onShownClipChanged: root.selectedKey = -1

    // Selects the key nearest to `fraction` of the clip (after an insert), or none.
    function selectKeyAt(fraction: real) {
        let best = -1
        let distance = 2
        for (const k of TimeRemap.keys) {
            if (Math.abs(k.x - fraction) < distance) {
                distance = Math.abs(k.x - fraction)
                best = k.index
            }
        }
        root.selectedKey = best
    }
    spacing: Theme.space(2)
    visible: TimeRemap.available

    RowLayout {
        Layout.fillWidth: true
        IconButton {
            objectName: "timeRemapExpand"
            compact: true
            iconName: root.expanded ? "chevron-down" : "chevron-right"
            tooltip: qsTr("Time remapping graphs")
            onClicked: root.expanded = !root.expanded
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Time remapping · Speed")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Toggle {
            objectName: "timeRemapEnable"
            checked: TimeRemap.enabled
            Accessible.name: qsTr("Enable time remapping")
            onToggled: TimeRemap.setEnabled(checked)
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.expanded
        spacing: Theme.space(2)
        RowLayout {
            Layout.fillWidth: true
            PillTabs {
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Source time"), qsTr("Speed")]
                currentIndex: root.speedView ? 1 : 0
                onActivated: index => root.speedView = index === 1
                Accessible.name: qsTr("Time remapping graph view")
            }
            IconButton {
                compact: true
                enabled: TimeRemap.enabled
                iconName: "plus"
                tooltip: qsTr("Add speed key at playhead")
                onClicked: TimeRemap.addKey(TimeRemap.playhead)
            }
            IconButton {
                compact: true
                iconName: "sliders-horizontal"
                tooltip: qsTr("Time remapping presets")
                onClicked: presets.popup()
                ContextMenu {
                    id: presets
                    ContextMenuItem { text: qsTr("Speed ramp in"); onTriggered: TimeRemap.preset("in") }
                    ContextMenuItem { text: qsTr("Speed ramp out"); onTriggered: TimeRemap.preset("out") }
                    ContextMenuItem { text: qsTr("Speed ramp in-out"); onTriggered: TimeRemap.preset("inOut") }
                    ContextMenuSeparator {}
                    ContextMenuItem { text: qsTr("Freeze frame at playhead"); onTriggered: TimeRemap.preset("hold") }
                    ContextMenuItem { text: qsTr("Reverse segment at playhead"); onTriggered: TimeRemap.preset("reverse") }
                }
            }
        }
        Rectangle {
            id: graph
            objectName: "timeRemapGraph"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(32)
            radius: Theme.radiusClip
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            clip: true
            activeFocusOnTab: true
            Accessible.role: Accessible.Chart
            Accessible.name: root.speedView ? qsTr("Speed in percent over timeline time") : qsTr("Source time over timeline time")
            Accessible.description: qsTr("Select a key, then use Left and Right to move it. Escape cancels a drag.")
            function yFor(value: real): real {
                return root.speedView ? Math.max(0, Math.min(height, (0.75 - value / 800) * height))
                     : height - (value - TimeRemap.sourceMin) / (TimeRemap.sourceMax - TimeRemap.sourceMin) * height
            }
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.accentStroke
                    strokeWidth: Theme.focusRingWidth
                    fillColor: "transparent"
                    PathPolyline {
                        path: (root.speedView ? TimeRemap.speedPoints : TimeRemap.sourcePoints)
                              .map(p => Qt.point(p.x * graph.width, graph.yFor(p.y)))
                    }
                }
            }
            Rectangle {
                x: TimeRemap.playhead * graph.width
                width: Theme.hairline
                height: parent.height
                color: Theme.playhead
            }
            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.space(1)
                text: root.speedView ? qsTr("600%") : qsTr("%1 s").arg(TimeRemap.sourceMax.toFixed(2))
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(1)
                text: root.speedView ? qsTr("−200%") : qsTr("%1 s").arg(TimeRemap.sourceMin.toFixed(2))
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Repeater {
                model: TimeRemap.keys
                delegate: Rectangle {
                    id: key
                    required property var modelData
                    width: Theme.space(2)
                    height: width
                    radius: Theme.radiusClip
                    x: modelData.x * graph.width - width / 2
                    y: graph.yFor(root.speedView ? modelData.speed : modelData.source) - height / 2
                    color: root.selectedKey === modelData.index ? Theme.accentStroke : Theme.textPrimary
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Speed key %1 at %2 seconds, %3 percent").arg(modelData.index + 1)
                        .arg((modelData.x * TimeRemap.duration).toFixed(2)).arg(modelData.speed.toFixed(0))
                    Accessible.onPressAction: {
                        root.selectedKey = key.modelData.index
                        graph.forceActiveFocus()
                    }
                }
            }
            MouseArea {
                id: graphPointer
                anchors.fill: parent
                enabled: TimeRemap.enabled
                property int dragKey: -1
                onPressed: mouse => {
                    graph.forceActiveFocus()
                    dragKey = -1
                    let distance = Theme.space(3)
                    for (const k of TimeRemap.keys) {
                        const d = Math.abs(k.x * width - mouse.x)
                        if (d < distance) { distance = d; dragKey = k.index }
                    }
                    root.selectedKey = dragKey
                    if (dragKey >= 0) TimeRemap.beginGesture()
                }
                onPositionChanged: mouse => {
                    if (pressed && dragKey >= 0) TimeRemap.moveKey(dragKey, mouse.x / width)
                }
                onReleased: { dragKey = -1; TimeRemap.endGesture() }
                onCanceled: { dragKey = -1; TimeRemap.cancelGesture() }
                onDoubleClicked: mouse => TimeRemap.addKey(mouse.x / width)
            }
            Keys.onEscapePressed: event => {
                graphPointer.dragKey = -1
                TimeRemap.cancelGesture()
                event.accepted = true
            }
            Keys.onShortcutOverride: event => {
                event.accepted = [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Delete,
                                  Qt.Key_Insert, Qt.Key_Escape].includes(event.key)
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    const keys = TimeRemap.keys
                    const index = keys.findIndex(k => k.index === root.selectedKey)
                    if (keys.length > 0)
                        root.selectedKey = keys[(index + (event.key === Qt.Key_Up ? -1 : 1) + keys.length) % keys.length].index
                    event.accepted = true
                    return
                }
                const key = TimeRemap.keys.find(k => k.index === root.selectedKey)
                if (key && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
                    TimeRemap.moveKey(key.index, key.x + (event.key === Qt.Key_Left ? -0.01 : 0.01))
                    event.accepted = true
                }
                if (key && event.key === Qt.Key_Delete) {
                    if (TimeRemap.removeKey(key.index))
                        root.selectedKey = -1
                    event.accepted = true
                }
                if (event.key === Qt.Key_Insert) {
                    if (TimeRemap.addKey(TimeRemap.playhead))
                        root.selectKeyAt(TimeRemap.playhead)
                    event.accepted = true
                }
            }
            FocusRing { visible: graph.activeFocus }
        }
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: root.speedView ? qsTr("Speed (%) · timeline (s)") : qsTr("Source (s) · timeline (s)")
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Text {
                text: TimeRemap.duration.toFixed(2) + qsTr(" s")
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
        PillButton {
            Layout.fillWidth: true
            text: qsTr("Key interpolation")
            enabled: root.selectedKey >= 0 && TimeRemap.enabled
            onClicked: interpolation.popup()
            ContextMenu {
                id: interpolation
                ContextMenuItem { text: qsTr("Linear"); onTriggered: TimeRemap.setInterpolation(root.selectedKey, "linear") }
                ContextMenuItem { text: qsTr("Hold"); onTriggered: TimeRemap.setInterpolation(root.selectedKey, "hold") }
                ContextMenuItem { text: qsTr("Ease in"); onTriggered: TimeRemap.setInterpolation(root.selectedKey, "easeIn") }
                ContextMenuItem { text: qsTr("Ease out"); onTriggered: TimeRemap.setInterpolation(root.selectedKey, "easeOut") }
            }
        }
        PillButton {
            Layout.fillWidth: true
            text: qsTr("Frame sampling · implemented")
            onClicked: sampling.popup()
            ContextMenu {
                id: sampling
                ContextMenuItem { text: qsTr("Frame sampling · implemented"); checkable: true; checked: true }
                ContextMenuItem { text: qsTr("Frame blending · not implemented"); enabled: false }
            }
        }
        Text {
            Layout.fillWidth: true
            text: TimeRemap.message !== "" ? TimeRemap.message : qsTr("Audio plays only in straight forward 100% segments; ramps, holds and reverse are silent.")
            wrapMode: Text.Wrap
            color: TimeRemap.message !== "" ? Theme.warning : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }
}
