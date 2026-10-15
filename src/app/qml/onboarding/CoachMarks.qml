pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The tour's coach marks (M5j onboarding): slice-style callouts that point at one part of the window at a time
// (Library → Timeline → Export). The rest of the window dims; the highlighted part stays live (clicks reach it), so the
// tour is interactive: opening the Export workspace on the last step completes it. The highlight follows its target
// through layout animations.
// Keyboard: the card takes focus; Right or Return goes on, Left goes back, Esc ends the tour. Focus goes back where it
// was when the tour ends.
FocusScope {
    id: root
    objectName: "coachMarks"

    /// [{ key, title, text, target (Item), action (button text; "" for none) }]
    property var steps: []
    property int step: -1
    readonly property bool active: step >= 0 && step < steps.length
    readonly property var current: active ? steps[step] : null
    property rect hole: Qt.rect(0, 0, 0, 0)
    property Item returnFocus: null
    readonly property var hostWindow: Window.window

    signal stepShown(string key)
    /// The step's action button (the Export step's "Open Export").
    signal actionRequested(string key)
    signal finished(bool completed)

    function start() {
        if (root.steps.length === 0)
            return
        root.returnFocus = root.hostWindow ? root.hostWindow.activeFocusItem : null
        root.step = 0
        root.show()
    }
    function next() {
        if (root.step + 1 >= root.steps.length) {
            root.finish(true)
            return
        }
        root.step += 1
        root.show()
    }
    function back() {
        if (root.step <= 0)
            return
        root.step -= 1
        root.show()
    }
    function finish(completed) {
        if (!root.active)
            return
        root.step = -1
        const back = root.returnFocus
        root.returnFocus = null
        if (back && back.visible && back.enabled)
            back.forceActiveFocus(Qt.OtherFocusReason)
        root.finished(completed)
    }
    function show() {
        root.stepShown(root.current.key)
        root.updateHole()
        Qt.callLater(() => {
            root.updateHole()
            if (root.active)
                card.forceActiveFocus(Qt.TabFocusReason)
        })
    }
    function updateHole() {
        const target = root.current ? root.current.target : null
        if (!target || !target.visible || target.width <= 0) {
            root.hole = Qt.rect(root.width / 2, root.height / 2, 0, 0)
            return
        }
        const p = target.mapToItem(root, 0, 0)
        const m = Theme.space(1)
        root.hole = Qt.rect(Math.max(0, p.x - m), Math.max(0, p.y - m),
                            Math.min(root.width, target.width + 2 * m), Math.min(root.height, target.height + 2 * m))
    }

    visible: active
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Tour")

    // Follow the target while workspaces and panels animate into place.
    Timer {
        running: root.active
        repeat: true
        interval: 50
        onTriggered: root.updateHole()
    }

    // The dim around the highlighted part; it swallows clicks and wheels so only the highlight is live.
    component Dim: Rectangle {
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: wheel => wheel.accepted = true
        }
    }
    Dim {
        x: 0
        y: 0
        width: root.width
        height: root.hole.y
    }
    Dim {
        x: 0
        y: root.hole.y + root.hole.height
        width: root.width
        height: Math.max(0, root.height - y)
    }
    Dim {
        x: 0
        y: root.hole.y
        width: root.hole.x
        height: root.hole.height
    }
    Dim {
        x: root.hole.x + root.hole.width
        y: root.hole.y
        width: Math.max(0, root.width - x)
        height: root.hole.height
    }

    // The highlight: an accent frame with the step number on a pill tag.
    Rectangle {
        objectName: "coachMarkFrame"
        x: root.hole.x
        y: root.hole.y
        width: root.hole.width
        height: root.hole.height
        visible: root.hole.width > 0
        color: "transparent"
        radius: Theme.radiusPanel
        border.width: Theme.focusRingWidth
        border.color: Theme.accentStroke

        Rectangle {
            x: Theme.space(4)
            y: -height / 2
            width: stepTag.implicitWidth + Theme.space(5)
            height: Theme.space(6)
            radius: height / 2
            color: Theme.accentFill
            Text {
                id: stepTag
                anchors.centerIn: parent
                text: qsTr("%1 / %2").arg(root.step + 1).arg(root.steps.length)
                color: Theme.accentOnFill
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
                font.weight: Font.Bold
            }
        }
    }

    // The callout: beside the highlight when there is room, else below or above it.
    FocusScope {
        id: card
        objectName: "coachMarkCard"
        readonly property real gap: Theme.space(4)
        readonly property real edge: Theme.space(3)
        readonly property bool beside: root.hole.x + root.hole.width + gap + width <= root.width - edge
                                       || root.hole.x - gap - width >= edge
        width: Math.min(Theme.space(84), root.width - 2 * edge)
        height: cardColumn.implicitHeight + 2 * Theme.space(5)
        x: {
            if (root.hole.x + root.hole.width + gap + width <= root.width - edge)
                return root.hole.x + root.hole.width + gap
            if (root.hole.x - gap - width >= edge)
                return root.hole.x - gap - width
            return Math.max(edge, Math.min(root.width - width - edge, root.hole.x + root.hole.width / 2 - width / 2))
        }
        y: {
            if (beside)
                return Math.max(edge, Math.min(root.height - height - edge, root.hole.y + root.hole.height / 2 - height / 2))
            if (root.hole.y + root.hole.height + gap + height <= root.height - edge)
                return root.hole.y + root.hole.height + gap
            return Math.max(edge, root.hole.y - gap - height)
        }
        Accessible.role: Accessible.Dialog
        Accessible.name: root.current ? root.current.title : ""
        Accessible.description: root.current ? root.current.text : ""

        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape || event.key === Qt.Key_Left
                                                           || event.key === Qt.Key_Right || event.key === Qt.Key_Return
                                                           || event.key === Qt.Key_Enter
        Keys.onEscapePressed: root.finish(false)
        Keys.onLeftPressed: root.back()
        Keys.onRightPressed: root.next()
        Keys.onReturnPressed: root.next()
        Keys.onEnterPressed: root.next()

        Behavior on x {
            enabled: !Theme.reduceMotion && root.active
            NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
        }
        Behavior on y {
            enabled: !Theme.reduceMotion && root.active
            NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut }
        }

        // A raised card with a hairline rim (design language v3: a popover's shape).
        Rectangle {
            anchors.fill: parent
            color: Theme.raised
            radius: Theme.radiusMenu
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }

        ColumnLayout {
            id: cardColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(5)
            spacing: Theme.space(2)

            RowLayout {
                spacing: Theme.space(2)
                SliceMark {
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    text: qsTr("Tour · step %1 of %2").arg(root.step + 1).arg(root.steps.length)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.Medium
                }
            }
            Text {
                objectName: "coachMarkTitle"
                Layout.fillWidth: true
                text: root.current ? root.current.title : ""
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textTitle + 2
                font.weight: Theme.weightTitle
                wrapMode: Text.Wrap

            }
            Text {
                objectName: "coachMarkText"
                Layout.fillWidth: true
                text: root.current ? root.current.text : ""
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                lineHeight: 1.15
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.space(2)
                spacing: Theme.space(2)

                PillButton {
                    objectName: "coachMarkSkipButton"
                    variant: "ghost"
                    compact: true
                    text: qsTr("End tour")
                    tooltip: qsTr("End the tour (Esc)")
                    onClicked: root.finish(false)
                }
                Item { Layout.fillWidth: true }
                PillButton {
                    objectName: "coachMarkBackButton"
                    visible: root.step > 0
                    variant: "ghost"
                    compact: true
                    text: qsTr("Back")
                    onClicked: root.back()
                }
                PillButton {
                    objectName: "coachMarkActionButton"
                    visible: root.current !== null && (root.current.action ?? "") !== ""
                    variant: "secondary"
                    compact: true
                    text: root.current ? (root.current.action ?? "") : ""
                    onClicked: root.actionRequested(root.current.key)
                }
                PillButton {
                    objectName: "coachMarkNextButton"
                    variant: "primary"
                    compact: true
                    text: root.step + 1 >= root.steps.length ? qsTr("Done") : qsTr("Next")
                    onClicked: root.next()
                }
            }
        }
    }
}
