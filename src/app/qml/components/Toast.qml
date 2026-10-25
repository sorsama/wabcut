import QtQuick
import QtQuick.Effects

// A toast (design language v3): a short message on a rounded card with a coloured dot that slides in at the
// bottom-right, then leaves after a few seconds. Anchor it to the window's bottom-right corner.
//   show(message)                      the accent dot, 4 s
//   show(message, color)               Theme.warning / Theme.danger mark a problem: it stays 7 s
//   show(message, color, { actionText, action, secondaryActionText, secondaryAction, timeoutMs, untilSeen })
//       UX rework: an action pill (Save As…, Show in Explorer, Undo…) that runs `action` and closes the toast, and a
//       close button; a toast with an action stays 8 s. `timeoutMs` sets the time instead.
//       0.4: a second action (`secondaryActionText` / `secondaryAction`, a ghost pill before the first: Open, then
//       "Show in Explorer"). `untilSeen`: the time runs only while the window is active (an export that ended while
//       the user was in another app waits for them).
// 0.4: an error (Theme.danger) is never replaced: a show() while an error shows waits its turn (first in, first out,
// `pending`; at most 5 - when full the oldest waiting message that is not an error goes) and shows when the error leaves
// (its time, ×, its action); messages behind it drain the same way. Warnings and notes replace each other as before.
// The same text and colour as the message shown restarts its time instead of waiting (and takes the new call's
// actions: two exports of the same file name each open their own). Option `replaceKey`: a running readout (the clip
// volume keys' "Volume +1 dB (now 3 dB)") replaces the shown or waiting message with the same key, here and in
// MessageLog, so holding a key never queues or logs thirty of them. Every show() goes into MessageLog (the Background
// tasks popover's Recent messages); an error that leaves on its timer counts as unseen there.
// While the pointer is over the toast it stays - once it moved there (or over it): a toast that appears under a resting
// pointer still leaves on time. Long messages wrap (up to three lines).
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
    /// 0.4: the second action (a ghost pill before the first; "" for none) and what it runs.
    property string secondaryActionText: ""
    property var secondaryAction: null
    readonly property bool hasSecondary: secondaryActionText !== "" && typeof secondaryAction === "function"
    /// 0.4: the shown message's tone ("info" | "success" | "warning" | "danger", MessageLog.toneOf) and its log id.
    property string tone: "info"
    property int entryId: 0
    /// 0.4: the shown message's `replaceKey` ("" for none).
    property string replaceKey: ""
    readonly property bool isError: root.tone === "danger"
    /// 0.4: the shown message's time runs only while the window is active.
    property bool untilSeen: false
    /// 0.4: the messages waiting behind an error, oldest first: { text, color, options, entryId, tone }.
    property var pending: []
    readonly property int pendingCount: root.pending.length
    readonly property int pendingLimit: 5
    readonly property bool windowActive: root.Window.active

    function show(message, color, options) {
        const o = options ?? ({})
        const kind = MessageLog.toneOf(color)
        const key = String(o.replaceKey ?? "")
        const id = MessageLog.addKeyed(message, kind, o.actionText ?? "", o.action ?? null, key)
        // The message shown, again: its time starts again with this call's actions (the earlier call's would open
        // another export's file); nothing new waits. A readout replaces its own earlier value the same way.
        if (root.shown && ((root.text === message && Qt.colorEqual(root.accent, color ?? Theme.accentStroke))
                           || (key !== "" && root.replaceKey === key && !root.isError))) {
            root.present(message, color, o, id, kind)
            return
        }
        // An error shows (or messages already wait behind one): this one waits its turn.
        if (root.shown && (root.isError || root.pending.length > 0)) {
            root.enqueue({ text: message, color: color, options: o, entryId: id, tone: kind })
            return
        }
        root.present(message, color, o, id, kind)
    }
    /// Closes it now; the next waiting message, if any, shows.
    function dismiss() {
        hideTimer.stop()
        root.next()
    }
    /// 0.4: closes it and drops every waiting message (waiting errors count as unseen in MessageLog).
    function dismissAll() {
        const old = root.pending
        root.pending = []
        for (let i = 0; i < old.length; ++i) {
            if (old[i].tone === "danger")
                MessageLog.missed(old[i].entryId)
        }
        hideTimer.stop()
        root.shown = false
    }
    /// The action pill: closes the toast, then runs the action.
    function runAction() {
        const run = root.action
        root.dismiss()
        if (typeof run === "function")
            run()
    }
    /// 0.4: the second pill: closes the toast, then runs its action.
    function runSecondaryAction() {
        const run = root.secondaryAction
        root.dismiss()
        if (typeof run === "function")
            run()
    }

    // Shows one message now (from show() or from the queue).
    function present(message, color, o, id, kind) {
        root.text = message
        root.accent = color ?? Theme.accentStroke
        root.tone = kind
        root.entryId = id
        root.actionText = o.actionText ?? ""
        root.action = o.action ?? null
        root.secondaryActionText = o.secondaryActionText ?? ""
        root.secondaryAction = o.secondaryAction ?? null
        root.untilSeen = o.untilSeen === true
        root.replaceKey = String(o.replaceKey ?? "")
        const problem = kind === "warning" || kind === "danger"
        root.timeout = o.timeoutMs > 0 ? o.timeoutMs : root.hasAction ? 8000 : problem ? 7000 : 4000
        root.shown = true
        root.startTime()
    }
    // The shown message left: the oldest waiting one shows, else the toast goes.
    function next() {
        const old = root.pending
        if (old.length === 0) {
            root.shown = false
            return
        }
        const head = old[0]
        const rest = []
        for (let i = 1; i < old.length; ++i)
            rest.push(old[i])
        root.pending = rest
        root.present(head.text, head.color, head.options, head.entryId, head.tone)
    }
    // Adds `item` to the queue (copied: never changed in place). Full: the oldest waiting message that is not an error
    // goes (all errors: the oldest error, which MessageLog keeps as unseen). The same message as the newest waiting
    // one takes its place; so does a readout with the same `replaceKey` as a waiting one (wherever it waits).
    function enqueue(item: var) {
        const old = root.pending
        const list = []
        for (let i = 0; i < old.length; ++i)
            list.push(old[i])
        if (list.length > 0 && list[list.length - 1].text === item.text && list[list.length - 1].tone === item.tone) {
            list[list.length - 1] = item
            root.pending = list
            return
        }
        const key = String(item.options.replaceKey ?? "")
        if (key !== "") {
            for (let i = 0; i < list.length; ++i) {
                if (String(list[i].options.replaceKey ?? "") === key) {
                    list[i] = item
                    root.pending = list
                    return
                }
            }
        }
        if (list.length >= root.pendingLimit) {
            let drop = 0
            for (let i = 0; i < list.length; ++i) {
                if (list[i].tone !== "danger") {
                    drop = i
                    break
                }
            }
            const gone = list[drop]
            list.splice(drop, 1)
            if (gone.tone === "danger")
                MessageLog.missed(gone.entryId)
        }
        list.push(item)
        root.pending = list
    }
    // A pointer that holds the toast holds the new message too; a message to be seen first (untilSeen) waits while the
    // window is not active; else it leaves after its time.
    function startTime() {
        if ((root.held && hover.hovered) || (root.untilSeen && !root.windowActive))
            hideTimer.stop()
        else
            hideTimer.restart()
    }
    onWindowActiveChanged: {
        if (!root.shown || !root.untilSeen)
            return
        if (!root.windowActive)
            hideTimer.stop()
        else if (!(root.held && hover.hovered))
            hideTimer.restart()
    }

    property bool shown: false
    /// The pointer holds the toast: it moved onto it, or over it, while it showed. Qt Quick marks an item that appears
    /// under a resting pointer as hovered at once; that alone does not hold it.
    property bool held: false
    onShownChanged: {
        if (!root.shown)
            root.held = false
    }

    readonly property bool showsActions: root.hasAction || root.hasSecondary || root.isError || root.pendingCount > 0
    implicitWidth: Math.min(root.hasSecondary ? Theme.space(170) : root.hasAction ? Theme.space(150) : Theme.space(110),
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
        onTriggered: {
            // An error nobody looked at: the Background tasks pill says so until its popover is opened.
            if (root.isError)
                MessageLog.missed(root.entryId)
            root.next()
        }
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
                    root.startTime()
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
            anchors.rightMargin: root.showsActions ? actions.width + Theme.space(5) : Theme.space(5)
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
            visible: root.showsActions
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(1)

            // 0.4: how many messages wait behind this one ("+2").
            Text {
                objectName: "toastPending"
                anchors.verticalCenter: parent.verticalCenter
                visible: root.pendingCount > 0
                rightPadding: Theme.space(1)
                //: Toast: how many more messages wait behind the one shown, e.g. "+2"
                text: qsTr("+%1").arg(root.pendingCount)
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
                Accessible.role: Accessible.StaticText
                Accessible.name: root.pendingCount === 1 ? qsTr("1 more message waiting")
                                                         : qsTr("%1 more messages waiting").arg(root.pendingCount)
            }
            // 0.4: the second thing to do (Show in Explorer next to Open): a ghost pill, so the first stands out.
            PillButton {
                objectName: "toastSecondaryAction"
                anchors.verticalCenter: parent.verticalCenter
                visible: root.hasSecondary
                compact: true
                variant: "ghost"
                text: root.secondaryActionText
                onClicked: root.runSecondaryAction()
            }
            // The one thing to do here: an accent pill (a raised one would not stand out on the card).
            PillButton {
                objectName: "toastAction"
                anchors.verticalCenter: parent.verticalCenter
                visible: root.hasAction
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
                tooltip: root.pendingCount > 0 ? qsTr("Dismiss (the next message shows)") : qsTr("Dismiss")
                onClicked: root.dismiss()
            }
        }
    }
}
