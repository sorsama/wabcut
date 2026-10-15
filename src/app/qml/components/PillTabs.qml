pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Tabs (design language v3; the name is v1's, the API is unchanged). Two looks:
//   * segmented (default): a sunken pill track with a sliding pill plate in the accent fill (black text on it);
//   * `tabs: true`: text tabs for a panel's views, the current one on a sliding hover-coloured pill.
// The indicator springs (no spring under reduce motion). `model` is a list of strings or of { label, icon }
// objects. Segments share the width equally while every label fits that way; otherwise each is as wide as its label
// plus an equal share of the room left, and when even that does not fit they shrink in proportion and the labels elide.
// Keyboard: Tab focuses the whole control, Left/Right (and Home/End) change the current tab.
Control {
    id: root

    property var model: []
    property int currentIndex: 0
    property bool compact: false
    // Text tabs with an underline (a panel's views) instead of the segmented plate.
    property bool tabs: false
    readonly property int count: model.length
    /// The average segment width (the segments themselves are `segmentWidths`).
    readonly property real segmentWidth: count > 0 ? (availableWidth / count) : 0

    // Each label's own width (icon, text and the segment's inner margins), then the widths the segments get.
    readonly property real labelPadding: Theme.space(compact ? 3 : 4)
    readonly property int labelSize: compact ? Theme.textSm : Theme.textMd
    FontMetrics {
        id: metrics
        font.family: Theme.fontSans
        font.pixelSize: root.labelSize
        font.weight: Theme.weightLabel
    }
    readonly property var naturalWidths: {
        const out = []
        for (let i = 0; i < root.count; ++i) {
            const item = root.model[i]
            const label = typeof item === "string" ? item : (item?.label ?? "")
            const icon = typeof item === "string" ? "" : (item?.icon ?? "")
            out.push(Math.ceil(metrics.advanceWidth(label)) + (icon !== "" ? Theme.iconSm + Theme.space(1.5) : 0)
                     + 2 * root.labelPadding)
        }
        return out
    }
    readonly property real naturalTotal: naturalWidths.reduce((a, b) => a + b, 0)
    readonly property var segmentWidths: {
        const room = root.availableWidth
        const n = root.count
        if (n === 0 || room <= 0)
            return root.naturalWidths.map(() => 0)
        if (Math.max(...root.naturalWidths) * n <= room)
            return root.naturalWidths.map(() => room / n)
        if (root.naturalTotal <= room)
            return root.naturalWidths.map(w => w + (room - root.naturalTotal) / n)
        return root.naturalWidths.map(w => w * room / root.naturalTotal)
    }
    function segmentX(index: int): real {
        let x = 0
        for (let i = 0; i < index; ++i)
            x += root.segmentWidths[i] ?? 0
        return x
    }

    // Emitted only for user interaction (click or key), like Slider.moved.
    signal activated(int index)

    function select(index) {
        const clamped = Math.max(0, Math.min(count - 1, index))
        if (clamped === currentIndex)
            return
        currentIndex = clamped
        activated(clamped)
    }

    implicitHeight: compact ? Theme.controlHeightSmall : Theme.controlHeight
    implicitWidth: Math.max(count * Theme.space(compact ? 16 : 20), naturalTotal) + leftPadding + rightPadding
    padding: tabs ? 0 : Theme.space(1) - Theme.hairline
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.PageTabList

    // Arrow keys, Home and End move between tabs here: claim them so the keymap does not move the playhead.
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Left || event.key === Qt.Key_Right
                                                       || event.key === Qt.Key_Home || event.key === Qt.Key_End
    Keys.onLeftPressed: root.select(root.currentIndex - 1)
    Keys.onRightPressed: root.select(root.currentIndex + 1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            root.select(0)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            root.select(root.count - 1)
            event.accepted = true
        }
    }

    background: Rectangle {
        radius: height / 2
        color: root.tabs ? "transparent" : Theme.sunken

        FocusRing {
            targetRadius: root.height / 2
            visible: root.visualFocus
        }
    }

    contentItem: Item {
        Rectangle {
            id: indicator
            x: root.segmentX(root.currentIndex)
            y: 0
            width: root.segmentWidths[root.currentIndex] ?? 0
            height: parent.height
            radius: height / 2
            color: root.tabs ? Theme.hover : Theme.accentFill
            visible: root.count > 0

            Behavior on x {
                enabled: !Theme.reduceMotion
                SpringAnimation {
                    spring: Theme.springStiffness
                    damping: Theme.springDamping
                    mass: Theme.springMass
                    epsilon: Theme.springEpsilon
                }
            }
        }

        Row {
            anchors.fill: parent

            Repeater {
                model: root.model

                delegate: AbstractButton {
                    id: tab

                    required property var modelData
                    required property int index

                    readonly property bool current: index === root.currentIndex
                    readonly property string label: typeof modelData === "string" ? modelData : (modelData.label ?? "")
                    readonly property string iconName: typeof modelData === "string" ? "" : (modelData.icon ?? "")
                    readonly property color foreground: current ? (root.tabs ? Theme.textPrimary : Theme.accentOnFill)
                                                      : hovered ? Theme.textPrimary : Theme.textSecondary
                    readonly property int labelWeight: Theme.weightLabel

                    width: root.segmentWidths[index] ?? 0
                    height: root.availableHeight
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    onClicked: root.select(index)

                    Accessible.role: Accessible.PageTab
                    Accessible.name: label
                    Accessible.selected: current

                    contentItem: Item {
                        Row {
                            id: labelRow
                            anchors.centerIn: parent
                            spacing: Theme.space(1.5)
                            // The label's room: the segment less its margins and the icon.
                            readonly property real textRoom: Math.max(0, tab.width - 2 * root.labelPadding
                                                                      - (tab.iconName !== "" ? Theme.iconSm + spacing : 0))

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: tab.iconName !== ""
                                name: tab.iconName
                                size: Theme.iconSm
                                color: tab.foreground
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, labelRow.textRoom)
                                text: tab.label
                                elide: Text.ElideRight
                                color: tab.foreground
                                font.family: Theme.fontSans
                                font.pixelSize: root.labelSize
                                font.weight: tab.labelWeight


                                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                            }
                        }
                    }

                    background: null

                    HoverHandler {
                        enabled: tab.enabled
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }
    }
}
