import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects

// Floating card that pops from its trigger (fade + scale from `transformOrigin`, ease-out, no bounce). Design language
// v3: a raised card, radius 20, a hairline ring and a soft shadow.
// Closes on Escape and on a click outside. Give it a `title` for an accent-dot header; children go below it.
// When it closes, keyboard focus goes back to the item that had it when it opened (M5j: focus is never lost), unless
// someone else took it meanwhile: a field the user clicked into, or a dialog the popover opened (review P1-8).
Popup {
    id: control

    property string title: ""
    default property alias bodyData: body.data
    // The item that had focus when the popover opened.
    property Item returnFocus: null
    // An item outside the popover the user gave focus to while it was open or closing (a click into a field), or null.
    property Item movedFocus: null

    onAboutToShow: {
        returnFocus = body.hostWindow ? body.hostWindow.activeFocusItem : null
        movedFocus = null
    }
    Connections {
        target: body.hostWindow
        enabled: control.visible // open, or running its exit transition
        function onActiveFocusItemChanged() {
            const win = body.hostWindow
            const item = win ? win.activeFocusItem : null
            if (item && item !== win.contentItem && !control.holds(item))
                control.movedFocus = item
        }
    }
    onClosed: {
        const target = returnFocus
        const moved = movedFocus
        returnFocus = null
        movedFocus = null
        const win = body.hostWindow
        const current = win ? win.activeFocusItem : null
        // Focus is free when it is on nothing (the window's content item, where Qt puts it when a focused popup
        // closes) or still in this popover.
        const free = current === null || (win !== null && current === win.contentItem) || control.holds(current)
        if (moved && moved.visible && moved.enabled) {
            // The user moved focus while the popover was open or closing: keep it there, also when Qt handed it to
            // the window as the popover finished closing (review P1-8).
            if (free && current !== moved)
                moved.forceActiveFocus(Qt.OtherFocusReason)
            return
        }
        if (free && target && target.visible && target.enabled)
            target.forceActiveFocus(Qt.PopupFocusReason)
    }

    // Whether `item` lies inside this popover.
    function holds(item: Item): bool {
        for (let p = item; p; p = p.parent) {
            if (p === control.contentItem || p === control.background || p === control.contentItem.parent) // + its popup item
                return true
        }
        return false
    }

    padding: Theme.pad
    margins: Theme.space(2)
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    transformOrigin: Item.BottomLeft

    contentItem: Column {
        spacing: Theme.space(3)
        Accessible.role: Accessible.Dialog
        Accessible.name: control.title

        SectionHeader {
            visible: control.title !== ""
            anchors.left: parent.left
            text: control.title
        }

        Item {
            id: body
            readonly property var hostWindow: Window.window
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }

    background: Rectangle {
        radius: Theme.radiusMenu
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong

        RectangularShadow {
            z: -1
            anchors.fill: parent
            radius: parent.radius
            offset.y: Theme.space(2)
            blur: Theme.space(8)
            color: Theme.shadow
        }
    }


    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }
}
