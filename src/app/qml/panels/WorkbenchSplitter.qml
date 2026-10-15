import QtQuick

// A workbench gutter you can drag (design language v2): place it over the gutter between two docked panels. It is
// invisible until hovered, then lights up with a 1px accent stroke; dragging reports how far the pointer moved
// (`dragged(delta)`, px along `orientation`). A double-click asks for the automatic size back (`reset`).
//
// 0.1.3, resize cost: a gutter drag used to relayout the whole workbench on every mouse move (the timeline's scene
// graph, the monitors' render targets and preview resolution, the Library grid), and with a gaming mouse that is
// hundreds of layouts a second. Now, by default (Preferences.liveResize off), a guide line follows the pointer and
// the layout changes once, on release (one `dragged` with the whole distance). With live resize on, the panels follow
// the pointer, at most one `dragged` per frame (the moves in between are summed).
Item {
    id: root

    property int orientation: Qt.Horizontal   // Qt.Horizontal: a vertical gutter dragged left/right
    /// The panels follow the pointer while it drags (else a guide does, and they resize on release).
    property bool live: Preferences.liveResize
    /// How far the guide may go (px from where the drag started); the layout clamps the same way.
    property real minDelta: -Infinity
    property real maxDelta: Infinity
    /// A drag is in progress (layouts can hold their animations meanwhile).
    readonly property bool active: area.pressed
    /// The distance dragged so far that the layout has not been given yet (the guide's offset while deferred).
    readonly property real pending: area.pending
    signal dragged(real delta)
    signal reset()
    signal finished()

    readonly property bool vertical: orientation === Qt.Vertical
    readonly property bool lit: area.containsMouse || area.pressed

    Rectangle {
        anchors.centerIn: parent
        width: root.vertical ? parent.width : Theme.hairline
        height: root.vertical ? Theme.hairline : parent.height
        color: Theme.accentStroke
        // While a deferred drag shows its guide elsewhere, the gutter itself stays faintly lit.
        opacity: root.lit ? (root.active && !root.live && root.pending !== 0 ? 0.4 : 1) : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    // The deferred drag's guide: where the gutter goes on release.
    Rectangle {
        objectName: "splitterGuide"
        visible: root.active && !root.live && root.pending !== 0
        x: root.vertical ? 0 : Math.round(root.width / 2 + root.pending - width / 2)
        y: root.vertical ? Math.round(root.height / 2 + root.pending - height / 2) : 0
        width: root.vertical ? root.width : Theme.focusRingWidth
        height: root.vertical ? Theme.focusRingWidth : root.height
        color: Theme.accentStroke
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        // Where the pointer was when the layout last heard of it, and the distance since (clamped to the limits).
        property real last: 0
        property real start: 0
        property real pending: 0
        function along(mouse) {
            const p = mapToItem(null, mouse.x, mouse.y)
            return root.vertical ? p.y : p.x
        }
        function flush() {
            if (pending === 0)
                return
            const delta = pending
            last += delta
            pending = 0
            root.dragged(delta)
        }
        onPressed: mouse => {
            last = along(mouse)
            start = last
            pending = 0
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const total = Math.max(root.minDelta, Math.min(root.maxDelta, along(mouse) - start))
            pending = start + total - last
        }
        onReleased: {
            flush()
            root.finished()
        }
        onCanceled: {
            // Deferred: nothing was applied yet, so a cancelled drag changes nothing.
            pending = 0
            root.finished()
        }
        onDoubleClicked: root.reset()

        // 0.1.3: the Boxicons resize cursor (EditCursors), like the timeline's.
        CursorArea {
            objectName: "splitterCursor"
            anchors.fill: parent
            shape: root.vertical ? "resizeV" : "resizeH"
        }
    }

    // Live: one layout per frame with whatever the pointer moved since the last one.
    FrameAnimation {
        running: root.live && area.pressed
        onTriggered: area.flush()
    }
}
