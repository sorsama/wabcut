pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC // qualified: unqualified, its ContextMenu would hide the app's

// ● MASKS (I.6, docs/masks.md) in Effect Controls, for the clip the panel shows (Masks singleton):
//   * where new masks go: the clip's opacity mask (cut-outs) or one of its video effects, the colour grade included
//     (the effect then changes only the masked area: a local correction);
//   * the monitor tools that draw them - ellipse, rectangle, pen (click for corners, drag for curves, click the first
//     point to close) - or a centred ellipse with +;
//   * every mask of the target with its mode, invert, on/off and delete; the selected one's feather, expansion and
//     opacity (one undo step per drag; a key at the playhead when animated) and its handles on the monitor;
//   * the tracker: to the start, one frame back, one frame forward, to the end, with its progress and a per-frame
//     confidence strip; it stops where the picture is lost. Every result is ordinary keyframes, one undo step.
Rectangle {
    id: root
    objectName: "maskSection"

    property bool expanded: true
    /// The owner caps the section (the effect list keeps its room); the rest scrolls.
    property real maximumHeight: Number.POSITIVE_INFINITY
    readonly property var current: Masks.current
    readonly property bool hasSelection: Masks.selected !== ""

    implicitHeight: Math.min(column.implicitHeight + Theme.space(3), maximumHeight)
    clip: true
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.border

    function targetName() {
        for (let i = 0; i < Masks.targets.length; ++i) {
            if (Masks.targets[i].effect === Masks.target)
                return Masks.targets[i].name
        }
        return qsTr("Opacity mask")
    }
    function modeIndex(mode) {
        return mode === "subtract" ? 1 : mode === "intersect" ? 2 : 0
    }

    // While tracking, keep the tracker's progress and confidence in view.
    Connections {
        target: Masks
        function onTrackingChanged() {
            if (Masks.tracking)
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
        }
    }

    Flickable {
        id: flick
        objectName: "maskSectionFlick"
        anchors.fill: parent
        anchors.margins: Theme.space(1.5)
        contentWidth: width
        contentHeight: column.implicitHeight
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        QC.ScrollBar.vertical: ThinScrollBar {}

    ColumnLayout {
        id: column
        width: flick.width
        spacing: Theme.space(1.5)

        // Header: collapse, title, target.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)

            IconButton {
                objectName: "maskSectionToggle"
                compact: true
                iconName: root.expanded ? "chevron-down" : "chevron-right"
                tooltip: root.expanded ? qsTr("Collapse masks") : qsTr("Expand masks")
                onClicked: root.expanded = !root.expanded
            }
            Text {
                text: qsTr("Masks")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Text {
                visible: Masks.masks.length > 0
                text: Masks.masks.length
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Item {
                Layout.fillWidth: true
            }
            PillButton {
                id: targetButton
                objectName: "maskTargetButton"
                compact: true
                variant: "ghost"
                iconName: "chevron-down"
                text: root.targetName()
                tooltip: qsTr("What the masks shape: the clip's opacity, or one of its effects")
                onClicked: targetMenu.popup(targetButton, 0, targetButton.height + Theme.space(1))

                ContextMenu {
                    id: targetMenu
                    Instantiator {
                        model: Masks.targets
                        delegate: ContextMenuItem {
                            required property var modelData
                            text: modelData.effect === "" ? qsTr("Opacity mask (cut out)") : qsTr("%1 (local)").arg(modelData.name)
                            checkable: true
                            checked: modelData.effect === Masks.target
                            onTriggered: Masks.target = modelData.effect
                        }
                        onObjectAdded: (index, object) => targetMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => targetMenu.removeItem(object)
                    }
                }
            }
        }

        // Tools.
        RowLayout {
            Layout.fillWidth: true
            visible: root.expanded
            spacing: Theme.space(1)

            IconButton {
                objectName: "maskToolEllipse"
                compact: true
                checkable: true
                checked: Masks.tool === "ellipse"
                iconName: "circle"
                tooltip: qsTr("Ellipse: drag on the program monitor")
                onClicked: Masks.tool = checked ? "ellipse" : ""
            }
            IconButton {
                objectName: "maskToolRectangle"
                compact: true
                checkable: true
                checked: Masks.tool === "rectangle"
                iconName: "square"
                tooltip: qsTr("Rectangle: drag on the program monitor")
                onClicked: Masks.tool = checked ? "rectangle" : ""
            }
            IconButton {
                objectName: "maskToolPen"
                compact: true
                checkable: true
                checked: Masks.tool === "pen"
                iconName: "pencil"
                tooltip: qsTr("Pen: click for corners, drag for curves, click the first point to close")
                onClicked: Masks.tool = checked ? "pen" : ""
            }
            Item {
                Layout.fillWidth: true
            }
            IconButton {
                objectName: "maskAdd"
                compact: true
                iconName: "plus"
                tooltip: qsTr("Add an ellipse mask in the middle")
                onClicked: Masks.addMask("ellipse")
            }
        }

        // AI Object Mask (AI-14): click a person or object on the monitor, track it through the shot.
        ObjectMaskControls {
            Layout.fillWidth: true
            visible: root.expanded
        }

        Text {
            Layout.fillWidth: true
            visible: root.expanded && Masks.masks.length === 0
            text: Masks.tool === "pen" ? qsTr("Click on the monitor to place points; click the first point to close the shape.")
                                       : qsTr("Draw a mask on the program monitor with a tool above, or add one with +.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // The target's masks.
        Repeater {
            model: root.expanded ? Masks.masks : []
            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property bool selected: modelData.key === Masks.selected
                objectName: "maskRow_" + modelData.key
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.rowHeight
                radius: Theme.radiusField
                color: selected ? Theme.accentWash : rowHover.hovered ? Theme.hover : "transparent"
                border.width: selected ? Theme.hairline : 0
                border.color: Theme.accentStroke
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name
                Accessible.selected: selected
                Accessible.onPressAction: Masks.selected = row.modelData.key
                activeFocusOnTab: true
                // The row's keys are its own while it has focus (Space would otherwise play).
                Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) === 0
                                                                   && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
                Keys.onSpacePressed: Masks.selected = row.modelData.key
                Keys.onReturnPressed: Masks.selected = row.modelData.key
                Keys.onEnterPressed: Masks.selected = row.modelData.key

                HoverHandler {
                    id: rowHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: Masks.selected = row.modelData.key
                }
                FocusRing {
                    targetRadius: Theme.radiusField
                    visible: row.activeFocus
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(1)
                    spacing: Theme.space(1)

                    Icon {
                        name: row.modelData.shape === "rectangle" ? "square" : row.modelData.shape === "path" ? "pencil"
                            : row.modelData.shape === "matte" ? "sparkles" : "circle"
                        size: Theme.iconSm
                        color: row.modelData.enabled ? Theme.textSecondary : Theme.textDisabled
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        elide: Text.ElideRight
                        color: row.modelData.enabled ? Theme.textPrimary : Theme.textDisabled
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Icon {
                        visible: row.modelData.tracked
                        name: "crosshair"
                        size: Theme.iconXs
                        color: Theme.accentStroke
                    }
                    Text {
                        visible: row.modelData.mode !== "add"
                        text: row.modelData.mode === "subtract" ? qsTr("SUB") : qsTr("INT")
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    IconButton {
                        compact: true
                        checkable: true
                        checked: row.modelData.inverted
                        iconName: "blend"
                        tooltip: checked ? qsTr("Inverted: covers the outside") : qsTr("Invert")
                        onClicked: Masks.setInverted(row.modelData.key, checked)
                    }
                    IconButton {
                        compact: true
                        iconName: row.modelData.enabled ? "eye" : "eye-off"
                        tooltip: row.modelData.enabled ? qsTr("Turn the mask off") : qsTr("Turn the mask on")
                        onClicked: Masks.setEnabled(row.modelData.key, !row.modelData.enabled)
                    }
                    IconButton {
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Delete the mask")
                        onClicked: Masks.removeMask(row.modelData.key)
                    }
                }
            }
        }

        // The selected mask.
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.expanded && root.hasSelection
            spacing: Theme.space(1)

            PillTabs {
                objectName: "maskModeTabs"
                Layout.fillWidth: true
                Accessible.name: qsTr("Mask mode")
                compact: true
                model: [qsTr("Add"), qsTr("Subtract"), qsTr("Intersect")]
                currentIndex: root.modeIndex(root.current.mode)
                onActivated: index => Masks.setMode(Masks.selected, ["add", "subtract", "intersect"][index])
            }
            PropSlider {
                objectName: "maskFeather"
                Layout.fillWidth: true
                label: qsTr("Feather")
                from: 0
                to: 0.5
                stepSize: 0.005
                decimals: 3
                value: root.current.feather !== undefined ? root.current.feather : 0
                defaultValue: 0
                onValueModified: v => Masks.setValue("feather", v)
                onGestureEnded: Masks.endEdit()
            }
            PropSlider {
                objectName: "maskExpansion"
                Layout.fillWidth: true
                label: qsTr("Expansion")
                from: -0.25
                to: 0.25
                origin: 0
                stepSize: 0.005
                decimals: 3
                value: root.current.expansion !== undefined ? root.current.expansion : 0
                defaultValue: 0
                onValueModified: v => Masks.setValue("expansion", v)
                onGestureEnded: Masks.endEdit()
            }
            PropSlider {
                objectName: "maskOpacity"
                Layout.fillWidth: true
                label: qsTr("Opacity")
                unit: "%"
                from: 0
                to: 100
                value: root.current.opacity !== undefined ? root.current.opacity * 100 : 100
                defaultValue: 100
                onValueModified: v => Masks.setValue("opacity", v / 100)
                onGestureEnded: Masks.endEdit()
            }

            // Tracking (an object mask tracks with the Object tool's buttons above).
            RowLayout {
                Layout.fillWidth: true
                visible: root.current.shape !== "matte"
                spacing: Theme.space(1)

                Text {
                    text: qsTr("Track")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Item {
                    Layout.fillWidth: true
                }
                IconButton {
                    objectName: "maskTrackToStart"
                    compact: true
                    enabled: !Masks.tracking
                    iconName: "skip-back"
                    tooltip: qsTr("Track backward to the clip's start")
                    onClicked: Masks.track("backward", 0)
                }
                IconButton {
                    objectName: "maskTrackBack"
                    compact: true
                    enabled: !Masks.tracking
                    iconName: "step-back"
                    tooltip: qsTr("Track one frame backward")
                    onClicked: Masks.track("backward", 1)
                }
                IconButton {
                    objectName: "maskTrackForward"
                    compact: true
                    enabled: !Masks.tracking
                    iconName: "step-forward"
                    tooltip: qsTr("Track one frame forward")
                    onClicked: Masks.track("forward", 1)
                }
                IconButton {
                    objectName: "maskTrackToEnd"
                    compact: true
                    enabled: !Masks.tracking
                    iconName: "skip-forward"
                    tooltip: qsTr("Track forward to the clip's end")
                    onClicked: Masks.track("forward", 0)
                }
                IconButton {
                    objectName: "maskTrackCancel"
                    visible: Masks.tracking
                    compact: true
                    iconName: "x"
                    tooltip: qsTr("Stop tracking (nothing is changed)")
                    onClicked: Masks.cancelTrack()
                }
            }
            BeamProgress {
                objectName: "maskTrackProgress"
                Layout.fillWidth: true
                visible: Masks.tracking
                active: Masks.tracking
                value: Masks.trackProgress
                label: qsTr("Tracking %1 %").arg(Math.round(Masks.trackProgress * 100))
            }
            ConfidenceStrip {
                Layout.fillWidth: true
                visible: Masks.confidence.length > 0
                values: Masks.confidence
                total: Masks.trackTotal
            }
            Text {
                objectName: "maskTrackStatus"
                Layout.fillWidth: true
                visible: text !== ""
                text: Masks.trackStatus
                wrapMode: Text.Wrap
                color: Masks.confidence.length > 0 && Masks.confidence[Masks.confidence.length - 1] < 0 ? Theme.warning : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        Text {
            Layout.fillWidth: true
            visible: Masks.status !== ""
            text: Masks.status
            wrapMode: Text.Wrap
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
    }
}
