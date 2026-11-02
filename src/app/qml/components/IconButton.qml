pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Round icon-only button (design language v3). `variant`: "ghost" (default: transparent, raised on hover),
// "secondary" (a raised circle) or "primary" (the accent fill). When `checkable`, the checked state uses the accent
// fill with a black glyph. Press = scale 0.98 over 90 ms (snap-out), no overshoot.
// P1 round (#31): the tooltip also shows on keyboard focus (a key press hides it until the focus moves) and on a
// disabled button, where it says why (`disabledReason`: by default the keymap command's reason).
Button {
    id: control

    property string iconName: ""
    property string variant: "ghost"
    property string tooltip: ""
    // Keymap command id (docs/keymap.md); the tooltip shows its current shortcut from KeymapDispatcher.
    property string command: ""
    property string shortcut: command !== "" && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
    /// Why the button cannot be used now, shown in its tooltip while it is disabled. By default the reason its keymap
    /// `command` cannot run (CommandPalette.commandState: "Nothing to undo").
    property string disabledReason: control.command !== "" && !control.enabled
                                    ? (CommandPalette.commandState(control.command).reason ?? "") : ""
    /// The tooltip's text: the reason while disabled (after the button's name), else `tooltip`.
    readonly property string tipText: !control.enabled && control.disabledReason !== ""
                                      ? (control.tooltip !== "" ? qsTr("%1: %2").arg(control.tooltip).arg(control.disabledReason)
                                                                : control.disabledReason)
                                      : control.tooltip
    // A key pressed while the keyboard focus shows the tooltip hides it until the focus moves.
    property bool focusTipDismissed: false
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
    Accessible.description: !enabled ? disabledReason : ""
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
    // Sees the pointer on a disabled button too (handlers get hover there), for the tooltip; no cursor of its own.
    HoverHandler {
        id: hoverAll
    }

    Keys.onPressed: control.focusTipDismissed = true // the event goes on to the button
    onActiveFocusChanged: control.focusTipDismissed = false

    Loader {
        objectName: "iconButtonTip"
        active: control.tipText !== "" && (hoverAll.hovered || (control.visualFocus && !control.focusTipDismissed))
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.tipText
            shortcut: control.enabled ? control.shortcut : ""
        }
    }
}
