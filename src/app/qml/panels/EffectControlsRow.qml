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
// and, on Bezier keys, in/out handles (InspectorPanel's lane area drags them: Alt breaks the pair, Shift keeps one level).
// 0.4: a click on a section header selects it (Ctrl toggles, Shift extends; Premiere's Effect Controls) for Backspace,
// Cut / Copy / Paste and the header's right-click menu (EffectSectionMenu); a selected header has the accent outline.
// 0.6, a graphic's or title's Text section: a group caption ("group", with an add button for strokes), the Source
// Text box ("textarea": applied when it loses focus or on Ctrl+Enter; its stopwatch keys the text as hold keys), the
// Font button ("font": the panel's font picker) and colour rows ("color": a swatch for the panel's colour picker and
// the hex value; strokes have a remove button). Multi mode (several clips) shows a value the clips do not share as "—".
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
    /// 0.4: the header is selected (Backspace removes it, Ctrl+C copies it; EffectControls.selectedSections).
    required property bool sectionSelected
    /// 0.6, multi mode: the selected clips do not share this value.
    required property bool mixed

    /// 0.6: the panel opens its colour picker / font picker for this row, under `anchor`.
    signal colorRequested(Item anchor)
    signal fontRequested(Item anchor)

    // ---- From the panel ----
    /// Width of the keyframe lane on the right (0: no lane; the slider gets the room).
    property real laneWidth: 0
    readonly property bool hasLane: laneWidth > 0
    /// 0.5.1: the part of the clip the lane shows (0..1 of the clip; InspectorPanel's zoom).
    property real viewStart: 0
    property real viewSpan: 1
    /// A position in the clip (0..1: keys, the graph) in lane pixels.
    function laneX(position: real): real {
        return (position - viewStart) / viewSpan * laneWidth
    }
    readonly property var controls: ProjectController.effectControls
    readonly property bool header: kind === "header"
    readonly property bool group: kind === "group"
    readonly property bool textArea: kind === "textarea"
    /// 0.7 (card 044): the Dynamics Processing graph row (a whole-width picture under the effect's header).
    readonly property bool dynamics: kind === "dynamics"
    /// 0.7 (card #130): a line of explanation under a header (a Warp Stabilizer's gyro data and how it was synced).
    readonly property bool note: kind === "note"
    /// A row with a stopwatch and keyframe navigation (a number, or 0.6's Source Text).
    readonly property bool keyed: kind === "param" || kind === "textarea"
    /// 0.6: sections that are not effects of the clip (a text layer's, multi mode's) are not selected.
    readonly property bool selectableSection: header && sectionKind !== "transition" && sectionKind !== "text"
                                              && sectionKind !== "multi" && sectionKind !== "pinned"
    /// 0.7 (card #57): a favourite effect the clip does not have yet: a header with Add (EffectControls.addPinned).
    readonly property bool pinned: header && sectionKind === "pinned"
    readonly property string pinnedEffectId: pinned ? section.substring(4) : ""
    /// Choices with at most this many options are pills, more a menu (0.6: a Text section's are long words: three).
    readonly property int pillLimit: sectionKind === "text" ? 3 : 4
    /// 0.6 (2B): pills only when they fit on one line of the row; otherwise (a narrow panel, long names such as Glow's
    /// "Color Channels") the menu, so wrapped pills never spill over the next row.
    readonly property real pillsWidth: {
        if (kind !== "choice" || !options)
            return 0
        let total = 0
        for (let i = 0; i < options.length; ++i)
            total += Math.ceil(pillFont.advanceWidth(String(options[i].label))) + Theme.space(7)
        return total
    }
    readonly property real pillRoom: Math.min(Theme.space(70), paramRow.width - Theme.space(7) - Theme.propLabelWidth
                                                               - Theme.space(12))
    readonly property bool pillChoice: kind === "choice" && !!options && options.length <= pillLimit && pillsWidth <= pillRoom
    FontMetrics { // PillButton's text (the selected pill's weight, the widest)
        id: pillFont
        font.pixelSize: Theme.textMd
        font.weight: Theme.weightTitle
    }
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

    // A choice row's pick: the transition's alignment or effect, else a menu parameter (0.2: Premiere's popups, the
    // option's value in UI units).
    function chooseOption(value) {
        if (rowKey === "t:alignment") {
            controls.setTransitionAlignment(value)
        } else if (rowKey === "t:effect") {
            controls.setTransitionEffect(value)
        } else {
            controls.setValue(rowKey, Number(value))
            controls.endEdit()
        }
    }
    readonly property string optionLabel: {
        if (kind !== "choice" || !options)
            return ""
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === text)
                return options[i].label
        }
        return text
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
    height: header ? Theme.controlHeight + Theme.space(1)
          : dynamics ? Math.ceil(dynamicsView.implicitHeight) + Theme.space(2)
          : note ? Math.ceil(noteLabel.implicitHeight) + Theme.space(2)
          : showGraph ? Theme.controlHeight + Theme.space(24)
          : textArea ? Theme.controlHeight + Theme.space(18)
          : group ? Theme.controlHeight - Theme.space(1)
          : Theme.controlHeight
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
            visible: row.hasLane && !row.dynamics
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: row.header ? Theme.space(2) : 1
            anchors.bottomMargin: row.header ? Theme.space(2) : 1
            width: row.laneWidth
            // A zoomed lane cuts off what is outside the view (unzoomed, the end keys may stand half outside it).
            clip: row.viewSpan < 1
            radius: row.header ? Theme.radiusClip : 0
            color: row.header ? (row.sectionKind === "transition" ? Theme.accentWash : Theme.raised) : "transparent"
            opacity: row.effectEnabled ? 1 : Theme.disabledOpacity

            Rectangle {
                visible: !row.header && !row.showGraph && !row.group
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
                    x: Math.round(row.laneX(modelData.x) - width / 2)
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
                // 0.5.1: the default renderer, multisampled. The curve renderer drew nothing for a polyline with a
                // straight run (a linear key's flat stretch), so linear curves showed no line.
                Shape {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.samples: 4
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
                                    points.push(Qt.point(row.laneX(flat[i]), flat[i + 1] * graphArea.height))
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
                        readonly property real kx: row.laneX(modelData.x)
                        readonly property real ky: modelData.y * graphArea.height
                        readonly property color handleColor: modelData.broken ? Theme.warning : Theme.accentStroke

                        HandleLine {
                            visible: point.modelData.hasIn
                            fromX: point.kx
                            fromY: point.ky
                            toX: row.laneX(point.modelData.inX)
                            toY: point.modelData.inY * graphArea.height
                            color: point.handleColor
                        }
                        HandleLine {
                            visible: point.modelData.hasOut
                            fromX: point.kx
                            fromY: point.ky
                            toX: row.laneX(point.modelData.outX)
                            toY: point.modelData.outY * graphArea.height
                            color: point.handleColor
                        }
                        HandleKnob {
                            objectName: "handleIn"
                            visible: point.modelData.hasIn
                            atX: row.laneX(point.modelData.inX)
                            atY: point.modelData.inY * graphArea.height
                            border.color: point.handleColor
                        }
                        HandleKnob {
                            objectName: "handleOut"
                            visible: point.modelData.hasOut
                            atX: row.laneX(point.modelData.outX)
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
            id: headerBox
            objectName: row.header ? "effectHeader_" + row.section : ""
            visible: row.header
            anchors.left: parent.left
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: row.hasLane ? Theme.space(2) : 0
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.controlHeight - Theme.space(1)
            radius: Theme.radiusField
            color: row.dragging ? Theme.hover : row.sectionSelected ? Theme.accentWash : Theme.raised
            border.width: row.sectionSelected ? Theme.selectionWidth : Theme.hairline
            border.color: row.dragging || row.sectionSelected ? Theme.accentStroke : Theme.border
            Accessible.role: Accessible.ListItem
            Accessible.name: row.title
            Accessible.selectable: row.selectableSection
            Accessible.selected: row.sectionSelected
            Accessible.onPressAction: {
                if (row.selectableSection)
                    row.controls.selectSection(row.section, 0)
            }

            // 0.4: a click selects the section (Ctrl/Cmd toggles it, Shift selects from the anchor) and gives Effect
            // Controls the keys (Backspace, Ctrl+C / X / V, Esc). The buttons on the header keep their own clicks.
            TapHandler {
                id: sectionSelect
                acceptedButtons: Qt.LeftButton
                enabled: row.selectableSection
                onTapped: {
                    row.ListView.view?.forceActiveFocus(Qt.MouseFocusReason)
                    row.controls.selectSection(row.section, sectionSelect.point.modifiers)
                }
            }

            // 0.2: right-click a clip's effect for its menu (EffectSectionMenu); 0.4: an unselected header is selected
            // alone first, a selected one keeps the selection (Cut / Copy / Clear act on it).
            TapHandler {
                id: sectionTap
                acceptedButtons: Qt.RightButton
                enabled: row.selectableSection
                onTapped: point => {
                    row.ListView.view?.forceActiveFocus(Qt.MouseFocusReason)
                    if (!row.sectionSelected)
                        row.controls.selectSection(row.section, 0)
                    sectionMenu.active = true
                    const menu = sectionMenu.item as EffectSectionMenu
                    menu.popupAt(sectionTap.parent, point.position.x, point.position.y, row.section)
                }
            }
            Loader {
                id: sectionMenu
                active: false
                sourceComponent: EffectSectionMenu {}
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(1)
                anchors.rightMargin: Theme.space(1)
                spacing: Theme.space(1)

                IconButton {
                    objectName: "sectionChevron"
                    visible: !row.pinned
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
                    id: sectionTitle
                    objectName: "sectionTitle"
                    Layout.fillWidth: true
                    // 0.7: the name of the effect keeps its room when a status (AI Restore's "Model needed" + Download) is beside it.
                    Layout.minimumWidth: Math.min(sectionTitle.implicitWidth, Theme.space(28)) // room for "AI Face Retouch" in full
                    text: row.known ? row.title : qsTr("%1 (not available)").arg(row.title)
                    elide: Text.ElideRight
                    color: row.effectEnabled ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    font.strikeout: !row.effectEnabled
                }

                // 0.7 (card #57): a pinned favourite: Add puts the effect on the clip; the x takes it off the Favorites.
                Text {
                    visible: row.pinned
                    text: qsTr("Favourite")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    objectName: "pinnedAdd"
                    visible: row.pinned
                    compact: true
                    variant: "secondary"
                    text: qsTr("Add")
                    tooltip: qsTr("Put %1 on this clip, ready to use").arg(row.title)
                    Accessible.name: qsTr("Add %1").arg(row.title)
                    onClicked: row.controls.addPinned(row.section)
                }
                IconButton {
                    objectName: "pinnedUnpin"
                    visible: row.pinned
                    compact: true
                    iconName: "x"
                    iconSize: Theme.iconSm
                    tooltip: qsTr("Unpin %1 (remove it from Favorites)").arg(row.title)
                    onClicked: EffectPresets.setFavorite("effect:" + row.pinnedEffectId, false)
                }

                // P.2: Warp Stabilizer / Rolling Shutter Repair: analysis state and Analyze.
                Loader {
                    id: stabilizerLoader
                    active: row.header && row.sectionKind === "effect"
                    // Not item.visible: that is false while this Loader is hidden (a binding loop that never shows it).
                    visible: status === Loader.Ready && (stabilizerLoader.item as StabilizerStatus).info.stabilizer === true
                    sourceComponent: StabilizerStatus { section: row.section }
                }

                // 0.7: AI Restore / AI Denoise: models, Process and the state of the clip's processed pictures.
                Loader {
                    id: aiVideoLoader
                    active: row.header && row.sectionKind === "effect"
                    visible: status === Loader.Ready && (aiVideoLoader.item as AiEffectStatus).info.ai === true
                    sourceComponent: AiEffectStatus { section: row.section }
                }

                // 0.7: Morph Cut's face detector (Face Aware): ready, or Download.
                Loader {
                    id: morphFacesLoader
                    active: row.header && row.sectionKind === "transition"
                    visible: status === Loader.Ready && (morphFacesLoader.item as MorphFacesStatus).info.morphCut === true
                    sourceComponent: MorphFacesStatus {}
                }

                PluginBadge {
                    kind: row.pluginInfo.kind ?? ""
                    detail: row.pluginInfo.text ?? ""
                    missedBlocks: row.pluginInfo.missedBlocks ?? 0
                }

                IconButton {
                    objectName: "editPluginButton"
                    visible: row.isPlugin && row.pluginInfo.kind !== "missing" && row.pluginInfo.openfx !== true // an OpenFX plug-in has no window
                    enabled: row.pluginInfo.hasEditor === true
                    compact: true
                    iconName: "external-link"
                    iconSize: Theme.iconSm
                    tooltip: row.pluginInfo.hasEditor === true ? qsTr("Edit plugin… (%1)").arg(row.pluginInfo.name ?? row.title)
                                                               : qsTr("%1 has no editor window").arg(row.pluginInfo.name ?? row.title)
                    onClicked: Plugins.openEditor(row.section) // an error shows as Plugins.message
                }

                // P.2: Motion's direct manipulation in the Program Monitor (Premiere: select Motion).
                IconButton {
                    objectName: "motionDirectButton"
                    visible: row.section === "motion"
                    compact: true
                    checkable: true
                    checked: ProgramMotion.engaged && ProgramMotion.mode === "transform"
                    iconName: "scaling"
                    iconSize: Theme.iconSm
                    tooltip: qsTr("Move, scale and rotate in the Program Monitor")
                    onClicked: {
                        ProgramMotion.mode = "transform"
                        ProgramMotion.engaged = checked
                        checked = Qt.binding(() => ProgramMotion.engaged && ProgramMotion.mode === "transform")
                    }
                }
                // P.2: Premiere's crop direct manipulation, on a Crop effect.
                IconButton {
                    objectName: "cropDirectButton"
                    visible: row.sectionKind === "effect" && ProgramMotion.isCropSection(row.section)
                    compact: true
                    checkable: true
                    checked: ProgramMotion.engaged && ProgramMotion.mode === "crop"
                    iconName: "crop"
                    iconSize: Theme.iconSm
                    tooltip: qsTr("Crop in the Program Monitor")
                    onClicked: {
                        ProgramMotion.toggleCrop()
                        checked = Qt.binding(() => ProgramMotion.engaged && ProgramMotion.mode === "crop")
                    }
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

        // ---- 0.6: a group caption in a Text section (Source Text, Appearance, Strokes, Transform) ----
        RowLayout {
            visible: row.group
            anchors.left: parent.left
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: row.hasLane ? Theme.space(2) : 0
            anchors.leftMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(1)

            Text {
                objectName: "effectGroupLabel"
                Layout.fillWidth: true
                text: row.title
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            IconButton {
                objectName: "groupAction_" + row.text
                visible: row.group && row.text !== ""
                compact: true
                // 0.6 (card #71): the Master Clip caption turns the master clip effects off (or on) for this clip.
                readonly property bool masterSwitch: row.text === "masterOff" || row.text === "masterOn"
                checkable: masterSwitch
                checked: row.text === "masterOff"
                iconName: masterSwitch ? "power" : "plus"
                iconSize: Theme.iconSm
                tooltip: row.text === "addStroke" ? qsTr("Add a stroke")
                         : row.text === "masterOff" ? qsTr("Turn the master clip effects off for this clip")
                         : row.text === "masterOn" ? qsTr("Turn the master clip effects on for this clip")
                         : row.title
                onClicked: row.controls.rowAction(row.rowKey, row.text)
            }
        }

        // ---- 0.6: Source Text (a text layer's or a title's): several lines, applied when it loses focus ----
        TextArea {
            id: sourceText
            objectName: "sourceTextField"
            visible: row.textArea
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(9)
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: Theme.space(2)
            anchors.top: parent.top
            anchors.topMargin: Theme.controlHeight
            height: Theme.space(16)
            text: row.text
            wrapMode: TextEdit.Wrap
            leftPadding: Theme.space(2)
            rightPadding: Theme.space(2)
            topPadding: Theme.space(1)
            bottomPadding: Theme.space(1)
            selectByMouse: true
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            opacity: row.effectEnabled ? 1 : Theme.disabledOpacity
            Accessible.name: row.title

            function apply() {
                if (text !== row.text)
                    row.controls.setEffectText(row.rowKey, text)
                text = Qt.binding(() => row.text)
            }
            onActiveFocusChanged: {
                if (!activeFocus && row.textArea)
                    apply()
            }
            Keys.onPressed: event => {
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                    apply()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    text = Qt.binding(() => row.text)
                    event.accepted = true
                }
            }
            background: Rectangle {
                radius: Theme.radiusField
                color: sourceText.activeFocus ? Theme.raised : Theme.sunken
                border.width: sourceText.activeFocus ? Theme.focusRingWidth : Theme.hairline
                border.color: sourceText.activeFocus ? Theme.accentStroke : Theme.border
            }
        }

        // ---- 0.7 (card #130): a note under a header, wrapped to the row ----
        Text {
            id: noteLabel
            objectName: row.note ? "effectNote_" + row.section : "effectNoteIdle"
            visible: row.note
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(3)
            anchors.right: row.hasLane ? lane.left : parent.right
            anchors.rightMargin: Theme.space(2)
            y: Theme.space(1)
            text: row.text
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            opacity: row.effectEnabled ? 1 : Theme.disabledOpacity
            Accessible.role: Accessible.StaticText
            Accessible.name: row.text
        }

        // ---- Parameter / switch / timecode / choice ----
        // ---- 0.7 (card 044): the Dynamics Processing graph (bands, transfer curve, meters, fields) ----
        DynamicsGraph {
            id: dynamicsView
            objectName: row.dynamics ? "dynamicsGraph_" + row.section : "dynamicsGraphIdle"
            visible: row.dynamics
            x: Theme.space(1)
            y: Theme.space(0.5)
            width: row.width - Theme.space(2)
            fx: row.dynamics && row.graph ? row.graph : ({})
            effectEnabled: row.effectEnabled
            presets: row.dynamics ? row.controls.dynamicsPresets() : []
            meterProvider: () => row.controls.dynamicsMeters(row.section)
            curveProvider: (mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points) =>
                           row.controls.dynamicsCurve(mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points)
            onValueEdited: (key, value) => row.controls.setValue(row.section + ":" + key, value)
            onEditFinished: row.controls.endEdit()
            onPresetRequested: name => row.controls.applyDynamicsPreset(row.section, name)
        }

        RowLayout {
            id: paramRow
            visible: !row.header && !row.group && !row.dynamics && !row.note
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
                visible: row.keyed && row.animatable
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
                visible: !(row.keyed && row.animatable)
                Layout.preferredWidth: Theme.space(7)
                Layout.preferredHeight: 1
            }

            Text {
                id: paramLabel
                objectName: "paramLabel"
                // 0.7 (QA): a row without a slider (a switch, a menu) has room to spare: its label takes it, so a long name
                // such as "Nearest Neighbor Scaling" is shown whole instead of cut to "Nearest N...".
                readonly property bool growing: row.kind === "toggle" || (row.kind !== "param" && !row.hasLane)
                Layout.preferredWidth: Theme.propLabelWidth
                Layout.fillWidth: paramLabel.growing
                text: row.title
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                Accessible.name: row.title
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
                visible: (row.kind !== "param" || row.hasLane) && !paramLabel.growing
                Layout.fillWidth: true
                Layout.preferredHeight: 1
            }

            NumberField {
                id: numberField
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
                Accessible.name: row.mixed ? qsTr("%1 (mixed)").arg(row.title) : row.title

                // 0.6, multi mode: the clips differ ("—" until a value is typed or dragged).
                Rectangle {
                    objectName: "mixedValue"
                    visible: row.mixed && !numberField.activeFocus
                    anchors.fill: parent
                    anchors.margins: Theme.hairline
                    radius: height / 2
                    color: Theme.raised
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("\u2014")
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textMd
                    }
                }
            }

            // 0.6: the Font row: the family; the panel's picker lists the installed fonts.
            PillButton {
                id: fontButton
                objectName: "fontButton"
                visible: row.kind === "font"
                Layout.maximumWidth: Theme.space(60)
                compact: true
                text: row.text + "  \u25BE"
                Accessible.name: qsTr("%1: %2").arg(row.title).arg(row.text)
                onClicked: row.fontRequested(fontButton)
            }

            // 0.6: a colour row: the swatch opens the panel's picker, the hex value can be typed.
            Swatch {
                id: colorSwatch
                objectName: "colorSwatch"
                visible: row.kind === "color"
                swatchColor: row.kind === "color" ? row.text : Theme.contentWhite
                checkable: false
                tooltip: row.description !== "" ? row.description : qsTr("Pick the %1 colour").arg(row.title)
                Accessible.name: qsTr("%1 colour").arg(row.title)
                onClicked: row.colorRequested(colorSwatch)
            }
            FieldInput {
                id: hexField
                objectName: "colorHexField"
                visible: row.kind === "color"
                Layout.preferredWidth: Theme.space(22)
                text: row.text
                font.family: Theme.fontMono
                Accessible.name: qsTr("%1, hex colour").arg(row.title)
                onEditingFinished: {
                    const t = text.trim()
                    const hex = t.startsWith("#") ? t : "#" + t
                    if (/^#[0-9a-fA-F]{6}$/.test(hex) && hex.toUpperCase() !== row.text) {
                        row.controls.setColor(row.rowKey, hex.toUpperCase())
                        row.controls.endEdit()
                    }
                    text = Qt.binding(() => row.text)
                }
            }
            IconButton {
                objectName: "rowRemove"
                visible: row.kind === "color" && row.removable
                compact: true
                iconName: "trash-2"
                iconSize: Theme.iconSm
                tooltip: qsTr("Remove %1").arg(row.title)
                onClicked: row.controls.rowAction(row.rowKey, "remove")
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

            // 0.2: a text row (Simple Text's text): applied when editing finishes.
            TextField {
                id: effectTextField
                objectName: "effectTextField"
                visible: row.kind === "text"
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.space(70)
                implicitHeight: Theme.controlHeightSmall
                text: row.text
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: Theme.space(2)
                rightPadding: Theme.space(2)
                selectByMouse: true
                font.pixelSize: Theme.textMd
                color: Theme.textPrimary
                selectionColor: Theme.accentFill
                selectedTextColor: Theme.accentOnFill
                Accessible.name: row.title
                onEditingFinished: {
                    if (text !== row.text)
                        row.controls.setEffectText(row.rowKey, text)
                    text = Qt.binding(() => row.text)
                }
                background: Rectangle {
                    radius: Theme.radiusField
                    color: effectTextField.activeFocus ? Theme.raised : Theme.sunken
                    border.width: effectTextField.activeFocus ? Theme.focusRingWidth : Theme.hairline
                    border.color: effectTextField.activeFocus ? Theme.accentStroke : Theme.border
                }
            }

            // A few options: pills; more (a transition's effect, eight directions): a menu.
            Flow {
                visible: row.pillChoice
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.space(70)
                spacing: Theme.space(1)
                layoutDirection: Qt.RightToLeft

                Repeater {
                    model: row.pillChoice ? row.options : []
                    delegate: PillButton {
                        required property var modelData
                        compact: true
                        text: modelData.label
                        variant: row.text === modelData.value ? "primary" : "secondary"
                        enabled: modelData.value !== "custom"
                        Accessible.checkable: true
                        Accessible.checked: row.text === modelData.value
                        onClicked: row.chooseOption(modelData.value)
                    }
                }
            }
            PillButton {
                id: choiceButton
                objectName: "choiceMenuButton"
                visible: row.kind === "choice" && !row.pillChoice
                compact: true
                text: row.optionLabel + "  ▾"
                Accessible.name: row.title
                onClicked: choiceMenu.popup(choiceButton, 0, choiceButton.height)

                ChoiceMenu {
                    id: choiceMenu
                    options: row.kind === "choice" && !row.pillChoice ? row.options : []
                    current: row.text
                    onChosen: value => row.chooseOption(value)
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
                visible: row.keyed && row.animated
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
                    visible: row.hasLane && row.kind === "param"
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
