import QtQuick
import QtQuick.Effects

// A toast (design language v3): a short message on a rounded card with a coloured dot that slides in at the
// bottom-right, then leaves after a few seconds. Anchor it to the window's bottom-right corner.
//   show(message)                      the accent dot, 4 s
//   show(message, color)               Theme.warning / Theme.danger mark a problem: it stays 7 s
//   show(message, color, { actionText, action, timeoutMs })
//       UX rework: an action pill (Save As…, Show in Explorer, Undo…) that runs `action` and closes the toast, and a
//       close button; a toast with an action stays 8 s. `timeoutMs` sets the time instead.
// A new show() replaces what is shown. While the pointer is over the toast it stays - once it moved there (or over it):
// a toast that appears under a resting pointer still leaves on time. Long messages wrap (up to three lines).
Item {
    id: root
    objectName: "toast"

    property string text: ""
    // The dot's colour (Theme.danger for a problem).
    property color accent: Theme.accentStroke
    property int timeout: 4000
    /// The action pill's text ("" for none) and what it runs.
    property string actionText: ""
    property var action: null
    readonly property bool hasAction: actionText !== "" && typeof action === "function"

    function show(message, color, options) {
        const o = options ?? ({})
        root.text = message
        root.accent = color ?? Theme.accentStroke
        root.actionText = o.actionText ?? ""
        root.action = o.action ?? null
        const problem = Qt.colorEqual(root.accent, Theme.warning) || Qt.colorEqual(root.accent, Theme.danger)
        root.timeout = o.timeoutMs > 0 ? o.timeoutMs : root.hasAction ? 8000 : problem ? 7000 : 4000
        root.shown = true
        // A pointer that holds the toast holds the new message too; else it leaves after its time.
        if (root.held && hover.hovered)
            hideTimer.stop()
        else
            hideTimer.restart()
    }
    /// Closes it now.
    function dismiss() {
        root.shown = false
        hideTimer.stop()
    }
    /// The action pill: closes the toast, then runs the action.
    function runAction() {
        const run = root.action
        root.dismiss()
        if (typeof run === "function")
            run()
    }

    property bool shown: false
    /// The pointer holds the toast: it moved onto it, or over it, while it showed. Qt Quick marks an item that appears
    /// under a resting pointer as hovered at once; that alone does not hold it.
    property bool held: false
    onShownChanged: {
        if (!root.shown)
            root.held = false
    }

    implicitWidth: Math.min(root.hasAction ? Theme.space(150) : Theme.space(110),
                            label.implicitWidth + label.anchors.leftMargin + label.anchors.rightMargin)
    implicitHeight: Math.max(Theme.controlHeight + Theme.space(2), label.implicitHeight + Theme.space(5))
    visible: opacity > 0
    opacity: shown ? 1 : 0
    // Only its action and close buttons take input (and the pointer over it holds it); nothing while it is gone.
    enabled: shown

    Behavior on opacity { NumberAnimation { duration: Theme.durState; easing.type: Easing.OutCubic } }

    Timer {
        id: hideTimer
        objectName: "toastHideTimer"
        interval: root.timeout
        onTriggered: root.shown = false
    }
    HoverHandler {
        id: hover
        objectName: "toastHover"
        // Where this hover began (`from`), taken from its first point (read in onPointChanged: the point may not be
        // updated yet when hovered changes).
        property bool anchored: false
        property point from: Qt.point(0, 0)
        onHoveredChanged: {
            hover.anchored = false
            if (!hover.hovered && root.held) {
                // The pointer left: the toast leaves after its time again.
                root.held = false
                if (root.shown)
                    hideTimer.restart()
            }
        }
        // The pointer moves over it (a hover from a resting pointer reports where it rests, again and again): it holds.
        onPointChanged: {
            if (!hover.hovered || root.held || !root.shown)
                return
            const at = hover.point.scenePosition
            if (!hover.anchored) {
                hover.anchored = true
                hover.from = at
            } else if (Math.abs(at.x - hover.from.x) + Math.abs(at.y - hover.from.y) >= 2) {
                root.held = true
                hideTimer.stop()
            }
        }
    }

    Item {
        width: root.width
        height: root.height
        x: root.shown || Theme.reduceMotion ? 0 : Theme.space(6)
        Behavior on x { NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut } }

        Rectangle {
            anchors.fill: parent
            color: Theme.section
            radius: Math.min(Theme.radiusCard, height / 2)
            border.width: Theme.hairline
            border.color: Theme.border

            RectangularShadow {
                z: -1
                anchors.fill: parent
                radius: parent.radius
                offset.y: Theme.space(2)
                blur: Theme.space(8)
                color: Theme.shadow
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(4)
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: width / 2
            color: root.accent
        }
        Text {
            id: label
            objectName: "toastText"
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.space(4) + Theme.dotSize + Theme.space(2.5)
            anchors.rightMargin: root.hasAction ? actions.width + Theme.space(5) : Theme.space(5)
            text: root.text
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold

            Accessible.role: Accessible.AlertMessage
            Accessible.name: root.text
        }
        Row {
            id: actions
            visible: root.hasAction
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(1)

            // The one thing to do here: an accent pill (a raised one would not stand out on the card).
            PillButton {
                objectName: "toastAction"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                variant: "primary"
                text: root.actionText
                onClicked: root.runAction()
            }
            IconButton {
                objectName: "toastClose"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "x"
                tooltip: qsTr("Dismiss")
                onClicked: root.dismiss()
            }
        }
    }
}
