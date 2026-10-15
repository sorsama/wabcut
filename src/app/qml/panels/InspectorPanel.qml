pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● EFFECT CONTROLS (M5d; was the Clip Editor): the selected clip's Transform, Opacity and Audio (pinned), its
// effect stack (drag to reorder, fx switch, reset, delete, collapse), every parameter with a stopwatch and keyframe
// navigation, and a keyframe lane on the right synced to the clip's range and the playhead. A selected transition
// shows its duration, alignment, kind and parameters instead.
//
// Lane: click a key to select it (Shift adds, Ctrl/Cmd toggles), drag to move the selected keys (Alt duplicates),
// drag on empty lane for a rubber band, right-click for the interpolation menu, Delete removes them; the ruler
// scrubs the playhead. Effects dropped here from the Effects panel are added to the clip (where dropped in the
// stack). Rows come from ProjectController.effectControls; every edit is one undo step.
Panel {
    id: root
    objectName: "effectControls"

    KeymapScope.scope: "effectControls"

    readonly property var controls: ProjectController.effectControls
    readonly property bool hasContent: controls.mode !== ""
    /// The keyframe lane is shown (Preferences.keyframeLane) and there is room for it.
    readonly property bool laneShown: Preferences.keyframeLane && hasContent && controls.mode === "clip"
                                      && list.width >= Theme.space(110)
    readonly property real laneWidth: laneShown ? Math.round(Math.min(Theme.space(90), list.width * 0.42)) : 0
    readonly property real laneX: list.width - laneWidth

    title: qsTr("Effect Controls")

    /// Explain this frame (M5k) takes the side panel.
    signal explainRequested()

    trailing: [
        IconButton {
            objectName: "explainFrameButton"
            compact: true
            iconName: "crosshair"
            tooltip: qsTr("Explain this frame: why the monitor shows what it shows")
            onClicked: root.explainRequested()
        },
        IconButton {
            objectName: "keyframeLaneToggle"
            compact: true
            checkable: true
            checked: Preferences.keyframeLane
            iconName: "diamond"
            tooltip: Preferences.keyframeLane ? qsTr("Hide the keyframe lane") : qsTr("Show the keyframe lane")
            onClicked: Preferences.keyframeLane = !Preferences.keyframeLane
        }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // Selected clip or transition
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight + Theme.space(2)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(3)
                spacing: Theme.space(2)

                Rectangle {
                    Layout.preferredWidth: Theme.space(2)
                    Layout.preferredHeight: Theme.space(5)
                    radius: Theme.radiusBadge
                    color: root.hasContent ? Theme.clipColor(root.controls.colorIndex) : Theme.textDisabled
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Text {
                        objectName: "effectControlsTitle"
                        Layout.fillWidth: true
                        text: root.hasContent ? root.controls.title : qsTr("No clip selected")
                        elide: Text.ElideRight
                        color: root.hasContent ? Theme.textPrimary : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textLg
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.hasContent
                        text: root.controls.subtitle
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }

                Rectangle {
                    visible: root.hasContent && root.controls.trackName !== ""
                    Layout.preferredWidth: trackLabel.implicitWidth + Theme.space(3)
                    Layout.preferredHeight: Theme.space(5)
                    radius: height / 2
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: Theme.border

                    Text {
                        id: trackLabel
                        anchors.centerIn: parent
                        text: root.controls.trackName
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }
            }
        }

        TimeRemapControls { Layout.fillWidth: true }

        // Lane ruler: the clip's first and last frame; click or drag to move the playhead.
        Item {
            id: ruler
            objectName: "keyframeRuler"
            visible: root.laneShown
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(5)

            Rectangle {
                x: root.laneX
                width: root.laneWidth
                height: parent.height
                radius: Theme.radiusClip
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.controls.laneStartText
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.controls.laneEndText
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    onPressed: mouse => root.controls.seekLane(mouse.x / width)
                    onPositionChanged: mouse => {
                        if (pressed)
                            root.controls.seekLane(mouse.x / width)
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                objectName: "effectControlsList"
                anchors.fill: parent
                clip: true
                model: root.controls
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                // Rows are the tab stops; the list itself does not take focus.
                activeFocusOnTab: false
                interactive: !laneArea.pressed
                ScrollBar.vertical: ThinScrollBar {}

                delegate: EffectControlsRow {
                    laneWidth: root.laneWidth
                }

                // Effect sections move with a spring when reordered; rows slide when sections open or close.
                move: Transition {
                    SpringAnimation {
                        property: "y"
                        spring: Theme.springStiffness
                        damping: Theme.springDamping
                        mass: Theme.springMass
                        epsilon: Theme.springEpsilon
                    }
                }
                moveDisplaced: Transition {
                    NumberAnimation { property: "y"; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
                }
                displaced: Transition {
                    NumberAnimation { property: "y"; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
                }
            }

            // Playhead across the lane.
            Rectangle {
                objectName: "lanePlayhead"
                visible: root.laneShown && root.controls.playheadPosition >= 0
                x: Math.round(root.laneX + root.controls.playheadPosition * root.laneWidth - width / 2)
                y: -ruler.height - Theme.space(2)
                width: Theme.playheadWidth
                height: parent.height + ruler.height + Theme.space(2)
                color: Theme.playhead
                opacity: 0.85
            }

            // Lane interaction (keys of every row): select, drag (Alt duplicates), rubber band, interpolation menu.
            MouseArea {
                id: laneArea
                objectName: "keyframeLane"
                visible: root.laneShown
                x: root.laneX
                width: root.laneWidth
                height: parent.height
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                preventStealing: true

                property real pressX: 0
                property real pressY: 0
                property bool draggingKeys: false
                property bool banding: false
                property rect band
                /// The value graph whose Bezier handle is being dragged, or null.
                property Item handleGraph: null

                /// The open value graph under a point of the lane: { row, graph }, or null.
                function graphAt(x, y) {
                    const p = mapToItem(list.contentItem, x, y)
                    const item = list.itemAt(Theme.space(2), p.y) as EffectControlsRow
                    return item && item.showGraph ? { row: item.rowKey, graph: item.graphItem } : null
                }

                function rowKeyAt(y) {
                    const p = mapToItem(list.contentItem, 0, y)
                    const i = list.indexAt(Theme.space(2), p.y)
                    return i >= 0 ? root.controls.keyAt(i) : ""
                }

                onPressed: mouse => {
                    root.forceActiveFocus(Qt.MouseFocusReason)
                    pressX = mouse.x
                    pressY = mouse.y
                    const key = rowKeyAt(mouse.y)
                    const tolerance = Theme.space(2) / width
                    if (mouse.button === Qt.RightButton) {
                        if (key !== "" && root.controls.pressKey(key, mouse.x / width, tolerance, 0))
                            keyMenu.popup(laneArea, mouse.x, mouse.y)
                        return
                    }
                    // A Bezier handle of an open value graph first.
                    const g = graphAt(mouse.x, mouse.y)
                    if (g) {
                        const q = mapToItem(g.graph, mouse.x, mouse.y)
                        if (root.controls.pressHandle(g.row, q.x / g.graph.width, q.y / g.graph.height,
                                                      Theme.space(2) / g.graph.width, Theme.space(2) / g.graph.height)) {
                            handleGraph = g.graph
                            return
                        }
                    }
                    const hit = key !== "" && root.controls.pressKey(key, mouse.x / width, tolerance, mouse.modifiers)
                    if (hit) {
                        draggingKeys = root.controls.beginKeyDrag((mouse.modifiers & Qt.AltModifier) !== 0)
                    } else {
                        banding = true
                        band = Qt.rect(mouse.x, mouse.y, 0, 0)
                    }
                }
                onPositionChanged: mouse => {
                    if (handleGraph) {
                        const q = mapToItem(handleGraph, mouse.x, mouse.y)
                        root.controls.dragHandle(q.x / handleGraph.width, q.y / handleGraph.height,
                                                 (mouse.modifiers & Qt.AltModifier) !== 0)
                    } else if (draggingKeys) {
                        root.controls.dragKeys((mouse.x - pressX) / width)
                    } else if (banding) {
                        band = Qt.rect(Math.min(pressX, mouse.x), Math.min(pressY, mouse.y),
                                       Math.abs(mouse.x - pressX), Math.abs(mouse.y - pressY))
                    }
                }
                onReleased: mouse => {
                    if (handleGraph) {
                        root.controls.endHandleDrag()
                        handleGraph = null
                    } else if (draggingKeys) {
                        root.controls.endKeyDrag()
                    } else if (banding && (band.width > 2 || band.height > 2)) {
                        const rows = []
                        for (let y = band.y; y <= band.y + band.height; y += Theme.space(2)) {
                            const key = rowKeyAt(y)
                            if (key !== "" && rows.indexOf(key) < 0)
                                rows.push(key)
                        }
                        root.controls.selectKeysInRange(rows, band.x / width, (band.x + band.width) / width, mouse.modifiers)
                    }
                    draggingKeys = false
                    banding = false
                }

                Rectangle {
                    visible: laneArea.banding
                    x: laneArea.band.x
                    y: laneArea.band.y
                    width: laneArea.band.width
                    height: laneArea.band.height
                    color: Theme.accentWash
                    border.width: Theme.hairline
                    border.color: Theme.accentStroke
                }
            }

            // Nothing selected: what this panel is for.
            Column {
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                visible: !root.hasContent
                spacing: Theme.space(2)

                EmptyState {
                    objectName: "effectControlsEmptyState"
                    width: parent.width
                    illustration: "effects"
                    text: qsTr("Select a clip or a transition to see its effects and keyframes. Drag effects here from the Effects panel.")
                }
            }

            // Effects dragged from the Effects panel: added to the clip, at the stack position under the pointer.
            DropArea {
                id: effectDrop
                objectName: "effectControlsDropArea"
                anchors.fill: parent
                keys: ["application/x-wabcut-effect"]
                property string message: ""
                property bool accepting: false

                onEntered: drag => {
                    const id = drag.getDataAsString("application/x-wabcut-effect")
                    accepting = root.controls.mode === "clip" && !root.controls.isTransition(id)
                    message = accepting ? "" : (root.controls.isTransition(id) ? qsTr("Drop transitions on an edit point in the timeline")
                                                                               : qsTr("Select a clip first"))
                    drag.accepted = accepting
                }
                onExited: {
                    accepting = false
                    message = ""
                }
                onDropped: drop => {
                    const id = drop.getDataAsString("application/x-wabcut-effect")
                    const p = mapToItem(list.contentItem, drop.x, drop.y)
                    const row = list.indexAt(Theme.space(2), p.y)
                    const kind = id.startsWith("wabcut.audio.") || id.startsWith("plugin.") ? "audio" : "video"
                    // M5m: an audio plugin goes through Plugins.apply, which also stores its name (the missing badge).
                    const error = id.startsWith("plugin.") ? Plugins.applyToSelection(id)
                                                           : root.controls.addEffect(id, row < 0 ? -1 : root.controls.stackIndexForRow(row, kind))
                    accepting = false
                    message = error
                    if (error === "")
                        drop.accept(Qt.CopyAction)
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: effectDrop.containsDrag && effectDrop.accepting
                radius: Theme.radiusField
                color: Theme.accentWash
                border.width: Theme.selectionWidth
                border.color: Theme.accentStroke
            }
        }

        // Channel Volume (A.9, M5n): the volume of each channel of the clip's sound (Premiere's Channel Volume).
        ChannelVolumeSection {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: root.controls.mode === "clip" && AudioChannels.volume.available === true
        }

        // Masks (I.6): the clip's opacity mask and its effects' local masks, the monitor tools and the tracker.
        MaskSection {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            maximumHeight: Math.max(Theme.space(30), root.height * 0.5)
            visible: root.controls.mode === "clip" && Masks.clip !== ""
        }

        // One-line feedback: why a drop or a transition edit did not go as asked.
        Text {
            objectName: "effectControlsNotice"
            Layout.fillWidth: true
            visible: text !== ""
            text: effectDrop.message !== "" ? effectDrop.message : root.controls.notice
            wrapMode: Text.Wrap
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // Delete / Backspace removes the selected keys while the panel has focus (the lane takes it on a press): the
    // panel claims the keys before the keymap's Delete (clips) sees them.
    Keys.onShortcutOverride: event => {
        if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && root.controls.selectedKeyCount > 0)
            event.accepted = true
    }
    Keys.onPressed: event => {
        if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && root.controls.selectedKeyCount > 0) {
            root.controls.deleteSelectedKeys()
            event.accepted = true
        }
    }

    // Right-click on a key: its interpolation (all selected keys), or delete them.
    KeyframeMenu {
        id: keyMenu
    }
}
