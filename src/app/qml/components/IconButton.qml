pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Round icon-only button (design language v3). `variant`: "ghost" (default: transparent, raised on hover),
// "secondary" (a raised circle) or "primary" (the accent fill). When `checkable`, the checked state uses the accent
// fill with a black glyph. Press = scale 0.98 over 90 ms (snap-out), no overshoot.
Button {
    id: control

    property string iconName: ""
    property string variant: "ghost"
    property string tooltip: ""
    // Keymap command id (docs/keymap.md); the tooltip shows its current shortcut from KeymapDispatcher.
    property string command: ""
    property string shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    property bool compact: false
    property int iconSize: compact ? Theme.iconSm : Theme.iconMd
    // Tint used while idle and unchecked; hover/checked states override it.
    property color iconColor: Theme.textSecondary
    // Background of the checked state (accent fill by default; e.g. Theme.danger for a mute toggle).
    property color checkedColor: Theme.accentFill
    property color checkedIconColor: Theme.accentOnFill

    readonly property bool filled: checked || variant === "primary"
    readonly property real roundRadius: Math.min(width, height) / 2

    implicitWidth: compact ? Theme.controlHeightSmall : Theme.controlHeight
    implicitHeight: implicitWidth
    padding: 0
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    display: AbstractButton.IconOnly

    scale: control.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    Accessible.name: tooltip
    Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
    Accessible.checkable: checkable
    Accessible.checked: checked

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: control.iconSize
            color: !control.enabled ? Theme.textDisabled
                 : control.filled ? control.checkedIconColor
                 : control.hovered ? Theme.textPrimary
                 : control.iconColor
        }
    }

    background: Rectangle {
        radius: control.roundRadius
        color: control.filled ? (control.down ? Theme.accentDeep : control.checkedColor)
             : control.down ? Theme.well
             : control.hovered ? (control.variant === "secondary" ? Theme.hover : Theme.raised)
             : control.variant === "secondary" ? Theme.raised : "transparent"
        opacity: control.enabled ? 1 : Theme.disabledOpacity

        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: control.roundRadius
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
