pragma ComponentBehavior: Bound
import QtQuick

// Motion graphics on the program monitor (C.13, M5o; bridge GraphicCanvas): a sibling of the monitor slot that fills
// the canvas (fractions of the canvas are fractions of this item), live while a graphic clip is selected and under the
// playhead.
//   * Pointing at a layer outlines it (dashed). A press picks as the monitor does (GraphicCanvas.pickPress: the object
//     picked last when it is under the pointer, else the topmost) and, when that is one of this graphic's layers,
//     drags it (a move, snapping to the canvas edges and centre, the safe margins and the other layers; Ctrl does not
//     snap). A press that picks anything else - a clip above or below, another graphic - passes through to the
//     monitor's pick area (MonitorPanel), as do presses on nothing.
//   * The selected layer's box, its handles - scale (corners; Shift scales freely), or the paragraph text box's edges,
//     rotate (the round handle above), the anchor point - and the snapping guides while a move snaps.
//   * A double-click goes one level deeper (GraphicCanvas.pickDoubleClick: a text layer is edited in place first, then
//     the object behind it is picked); `picked` hands the result to the monitor, which opens the editor. Enter edits
//     the selected text layer too.
//   * Keyboard (the overlay focused): arrows nudge the layer (Shift: 10 px), Tab / Shift+Tab pick the next or previous
//     layer, Enter edits a text layer, Esc cancels a drag or drops the selection.
// The monitor does not draw graphics until the render side rasterises draw lists; the handles work on the resolved
// bounds regardless.
Item {
    id: root
    objectName: "programGraphicOverlay"

    readonly property var selection: GraphicCanvas.selection
    readonly property bool hasSelection: selection.id !== undefined
    property int hovered: 0
    readonly property bool editing: editor.visible

    /// A double-click picked `result` (GraphicCanvas.pickDoubleClick): the monitor edits it when it says so.
    signal picked(var result)

    function pointsOf(quad) {
        const out = []
        for (let i = 0; i + 1 < (quad ?? []).length; i += 2)
            out.push(Qt.point(quad[i] * width, quad[i + 1] * height))
        return out
    }
    function quadOf(id) {
        const layers = GraphicCanvas.layers
        for (let i = 0; i < layers.length; ++i) {
            if (layers[i].id === id)
                return layers[i].quad
        }
        return []
    }
    function snapReach() {
        return width > 0 ? Theme.snapThreshold / width : 0
    }
    /// Starts editing the selected text layer in place.
    function startEditing() {
        if (!root.hasSelection || root.selection.kind !== "text" || root.selection.pinned)
            return false
        editor.open(root.selection.id, root.selection.editor ?? {}, root.selection.paragraph === true)
        return true
    }

    visible: GraphicCanvas.active
    activeFocusOnTab: false
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Graphic layers on the program monitor")
    Accessible.description: root.hasSelection ? qsTr("%1 selected: arrows move it, Tab picks the next layer").arg(root.selection.name)
                                              : qsTr("Click a layer to select it")

    // The safe margins the moves snap to are Theme's.
    Binding {
        target: GraphicCanvas
        property: "actionSafe"
        value: Theme.actionSafeInset
    }
    Binding {
        target: GraphicCanvas
        property: "titleSafe"
        value: Theme.titleSafeInset
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const outline = (points) => {
                if (points.length < 2)
                    return
                ctx.beginPath()
                ctx.moveTo(points[0].x, points[0].y)
                for (let i = 1; i < points.length; ++i)
                    ctx.lineTo(points[i].x, points[i].y)
                ctx.closePath()
                ctx.stroke()
            }
            // The layer under the pointer.
            if (root.hovered !== 0 && (!root.hasSelection || root.hovered !== root.selection.id)) {
                ctx.lineWidth = 1
                ctx.setLineDash([4, 3])
                ctx.strokeStyle = Theme.scrimTextMuted
                outline(root.pointsOf(root.quadOf(root.hovered)))
            }
            // The snapping guides.
            ctx.setLineDash([6, 4])
            ctx.lineWidth = 1
            ctx.strokeStyle = Theme.accentStroke
            for (let i = 0; i < GraphicCanvas.guides.length; ++i) {
                const g = GraphicCanvas.guides[i]
                ctx.beginPath()
                if (g.orientation === "vertical") {
                    ctx.moveTo(g.position * width, 0)
                    ctx.lineTo(g.position * width, height)
                } else {
                    ctx.moveTo(0, g.position * height)
                    ctx.lineTo(width, g.position * height)
                }
                ctx.stroke()
            }
            if (!root.hasSelection)
                return
            // The selected layer's box (dashed when pinned: it follows its target), the rotation stalk.
            ctx.setLineDash(root.selection.pinned ? [5, 4] : [])
            ctx.lineWidth = 1.5
            ctx.strokeStyle = Theme.accentStroke
            outline(root.pointsOf(root.selection.quad))
            ctx.setLineDash([])
            ctx.lineWidth = 1
            const handles = root.selection.handles ?? []
            for (let i = 0; i < handles.length; ++i) {
                const h = handles[i]
                if (h.kind === "rotate") {
                    ctx.beginPath()
                    ctx.moveTo(h.fromX * width, h.fromY * height)
                    ctx.lineTo(h.x * width, h.y * height)
                    ctx.stroke()
                }
            }
        }
        Connections {
            target: GraphicCanvas
            function onStateChanged() { canvas.requestPaint() }
            function onGuidesChanged() { canvas.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // Picking and moving this graphic's layers, outlining the layer under the pointer, double-clicks.
    MouseArea {
        id: body
        objectName: "graphicCanvasArea"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        enabled: !root.editing
        property bool moving: false

        onPositionChanged: mouse => {
            if (moving) {
                GraphicCanvas.dragTo(mouse.x / width, mouse.y / height, (mouse.modifiers & Qt.ShiftModifier) !== 0,
                                     (mouse.modifiers & Qt.ControlModifier) !== 0, root.snapReach())
                return
            }
            const id = GraphicCanvas.layerAt(mouse.x / width, mouse.y / height)
            if (id !== root.hovered) {
                root.hovered = id
                canvas.requestPaint()
            }
        }
        onExited: {
            if (root.hovered !== 0) {
                root.hovered = 0
                canvas.requestPaint()
            }
        }
        onPressed: mouse => {
            const fx = mouse.x / width
            const fy = mouse.y / height
            const target = GraphicCanvas.pickTarget(fx, fy)
            if (!(target.layer > 0) || target.clip !== GraphicCanvas.selectedClip) {
                mouse.accepted = false // another object, or none: the monitor's pick area
                return
            }
            GraphicCanvas.pickPress(fx, fy)
            root.forceActiveFocus()
            moving = GraphicCanvas.beginDrag("move", fx, fy)
        }
        onReleased: {
            if (moving)
                GraphicCanvas.endDrag()
            moving = false
        }
        onCanceled: {
            if (moving)
                GraphicCanvas.endDrag()
            moving = false
        }
        onDoubleClicked: mouse => {
            if (moving)
                GraphicCanvas.endDrag()
            moving = false
            root.picked(GraphicCanvas.pickDoubleClick(mouse.x / width, mouse.y / height))
        }
    }

    // The selected layer's handles (by count, so a drag's own updates keep the pressed handle alive).
    Repeater {
        model: root.hasSelection && !root.editing ? (root.selection.handles ?? []).length : 0
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property var modelData: index < (root.selection.handles ?? []).length ? root.selection.handles[index]
                                                                                          : ({kind: "", x: 0, y: 0})
            readonly property bool round: modelData.kind === "rotate"
            readonly property real size: 9
            objectName: "graphicHandle_" + modelData.kind
            x: modelData.x * root.width - width / 2
            y: modelData.y * root.height - height / 2
            width: size
            height: size
            radius: round ? size / 2 : Theme.radiusBadge / 2
            color: handleMouse.pressed ? Theme.accentFill : Theme.scrimText
            border.width: 1.5
            border.color: Theme.accentStroke
            Accessible.role: Accessible.Button
            Accessible.name: modelData.kind === "rotate" ? qsTr("Rotate the layer")
                           : modelData.kind === "box-right" || modelData.kind === "box-left" ? qsTr("Text box width")
                           : modelData.kind === "box-bottom" ? qsTr("Text box height")
                           : modelData.kind === "box-corner" ? qsTr("Text box size")
                           : qsTr("Scale the layer")

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                anchors.margins: -4
                cursorShape: handle.round ? Qt.PointingHandCursor
                           : handle.modelData.kind === "box-right" || handle.modelData.kind === "box-left" ? Qt.SizeHorCursor
                           : handle.modelData.kind === "box-bottom" ? Qt.SizeVerCursor : Qt.SizeFDiagCursor
                preventStealing: true
                onPressed: mouse => {
                    root.forceActiveFocus()
                    const p = mapToItem(root, mouse.x, mouse.y)
                    GraphicCanvas.beginDrag(handle.modelData.kind, p.x / root.width, p.y / root.height)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(root, mouse.x, mouse.y)
                    GraphicCanvas.dragTo(p.x / root.width, p.y / root.height, (mouse.modifiers & Qt.ShiftModifier) !== 0,
                                         (mouse.modifiers & Qt.ControlModifier) !== 0, root.snapReach())
                }
                onReleased: GraphicCanvas.endDrag()
                onCanceled: GraphicCanvas.endDrag()
            }
        }
    }

    // The anchor point: a ring with a cross; drag it to move the point the layer turns and scales around.
    Item {
        id: anchorHandle
        objectName: "graphicHandle_anchor"
        visible: root.hasSelection && !root.selection.pinned && !root.editing
        readonly property real size: 14
        x: (root.selection.anchor?.x ?? 0) * root.width - size / 2
        y: (root.selection.anchor?.y ?? 0) * root.height - size / 2
        width: size
        height: size
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Anchor point")

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 1.5
            border.color: anchorMouse.pressed ? Theme.accentFill : Theme.accentStroke
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width + 6
            height: 1
            color: Theme.accentStroke
        }
        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: parent.height + 6
            color: Theme.accentStroke
        }
        MouseArea {
            id: anchorMouse
            anchors.fill: parent
            anchors.margins: -3
            cursorShape: Qt.CrossCursor
            preventStealing: true
            onPressed: mouse => {
                root.forceActiveFocus()
                const p = mapToItem(root, mouse.x, mouse.y)
                GraphicCanvas.beginDrag("anchor", p.x / root.width, p.y / root.height)
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const p = mapToItem(root, mouse.x, mouse.y)
                GraphicCanvas.dragTo(p.x / root.width, p.y / root.height, false, true, 0)
            }
            onReleased: GraphicCanvas.endDrag()
            onCanceled: GraphicCanvas.endDrag()
        }
    }

    GraphicTextEditor {
        id: editor
        canvasWidth: root.width
        canvasHeight: root.height
        onCommitted: text => {
            GraphicCanvas.setLayerText(editor.layerId, text)
            root.forceActiveFocus()
        }
        onCancelled: root.forceActiveFocus()
    }

    // The keyboard path (the overlay has the focus after a click on a layer).
    Keys.onShortcutOverride: event => {
        if (!root.hasSelection || root.editing)
            return
        event.accepted = [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Tab, Qt.Key_Backtab, Qt.Key_Return,
                          Qt.Key_Enter, Qt.Key_Escape].includes(event.key)
    }
    Keys.onPressed: event => {
        if (root.editing)
            return
        const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 10 : 1
        switch (event.key) {
        case Qt.Key_Left:
            GraphicCanvas.nudge(-step, 0)
            break
        case Qt.Key_Right:
            GraphicCanvas.nudge(step, 0)
            break
        case Qt.Key_Up:
            GraphicCanvas.nudge(0, -step)
            break
        case Qt.Key_Down:
            GraphicCanvas.nudge(0, step)
            break
        case Qt.Key_Tab:
            GraphicCanvas.selectAdjacent(1)
            break
        case Qt.Key_Backtab:
            GraphicCanvas.selectAdjacent(-1)
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (!root.startEditing())
                return
            break
        case Qt.Key_Escape:
            if (GraphicCanvas.dragging)
                GraphicCanvas.cancelDrag()
            else
                GraphicCanvas.selectedLayer = 0
            break
        default:
            return
        }
        event.accepted = true
    }
}
