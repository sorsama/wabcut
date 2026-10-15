pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Push button (design language v3): a pill. `variant`: "primary" (the accent fill with black text, ExtraBold),
// "secondary" (a raised pill, bold) or "ghost" (transparent with a hairline outline). Optional `iconName` (an Icon
// name). Press = scale 0.98 over 90 ms (snap-out), no overshoot.
Button {
    id: control

    property string variant: "secondary"
    property string iconName: ""
    property string tooltip: ""
    // Keymap command id (docs/keymap.md); the tooltip shows its current shortcut from KeymapDispatcher.
    property string command: ""
    property string shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    property bool compact: false
    // v2's UV-beam line under a brand-moment primary button; v3 draws no beams, so it no longer changes the look.
    property bool beamAccent: false

    readonly property bool iconOnly: iconName !== "" && text === ""
    readonly property real pillRadius: height / 2
    readonly property color foreground: !enabled ? Theme.textDisabled
                                      : variant === "primary" ? Theme.accentOnFill
                                      : variant === "ghost" && !hovered ? Theme.textSecondary
                                      : Theme.textPrimary

    implicitHeight: compact ? Theme.controlHeightSmall : Theme.controlHeight
    implicitWidth: Math.max(implicitHeight, implicitContentWidth + leftPadding + rightPadding)
    padding: 0
    leftPadding: iconOnly ? 0 : Theme.space(compact ? 3 : 4)
    rightPadding: iconOnly ? 0 : Theme.space(compact ? 3 : 4)
    spacing: Theme.space(2)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd
    font.weight: variant === "primary" ? Theme.weightTitle : Theme.weightLabel

    scale: control.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    Accessible.name: text !== "" ? text : tooltip
    Accessible.description: tooltip

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName !== ""
                name: control.iconName
                size: control.compact ? Theme.iconSm : Theme.iconMd
                color: control.foreground
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.text !== ""
                text: control.text
                font: control.font
                color: control.foreground
            }
        }
    }

    background: Rectangle {
        implicitHeight: control.implicitHeight
        radius: control.pillRadius
        opacity: control.enabled ? 1 : Theme.disabledOpacity
        color: control.variant === "primary"
               ? (control.down ? Theme.accentDeep : control.hovered ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
               : control.variant === "secondary" ? (control.down ? Theme.well : control.hovered ? Theme.hover : Theme.raised)
               : (control.down ? Theme.raised : control.hovered ? Theme.hover : "transparent")
        border.width: control.variant === "ghost" ? Theme.hairline : 0
        border.color: Theme.borderStrong

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: control.pillRadius
            visible: control.visualFocus
        }
    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }

    Loader {
        active: control.hovered && control.tooltip !== ""
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.tooltip
            shortcut: control.shortcut
        }
    }
}
