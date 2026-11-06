import QtQuick
import QtQuick.Controls.Basic

// Minimize / maximize-restore / close pill buttons for the frameless window. Close turns red on hover.
// 0.7 (card 017): on Windows 11 the maximize pill is also the window's maximize button for the system: hovering it shows
// the Snap Layouts flyout. The system then sends the pointer to the window as non-client messages, which
// WindowChromeFilter turns back into `nativeHovered` / `nativePressed` (the hover and pressed look) and clicked().
Row {
    id: root

    property bool maximized: false
    /// 0.7 (card 017): the maximize pill answers the system's hit test (Snap Layouts) while this is true; the editor's bar
    /// turns it off while Home covers it.
    property bool snapEnabled: true

    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()

    spacing: Theme.space(1)

    component WindowButton: Button {
        id: button

        property string iconName: ""
        property string tooltip: ""
        property color hoverColor: Theme.hover
        property color hoverIconColor: Theme.textPrimary
        // The pointer is over / pressing the button as the system sees it (Snap Layouts; WindowChromeFilter sets them).
        property bool nativeHovered: false
        property bool nativePressed: false
        readonly property bool lit: button.hovered || button.nativeHovered

        implicitWidth: Theme.controlHeightSmall + Theme.space(3)
        implicitHeight: Theme.controlHeightSmall
        padding: 0
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.name: tooltip

        scale: down || nativePressed ? Theme.pressScale : 1.0
        Behavior on scale {
            NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
        }

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                name: button.iconName
                size: Theme.iconSm
                color: button.lit ? button.hoverIconColor : Theme.textSecondary
            }
        }

        background: Rectangle {
            radius: height / 2
            color: button.lit || button.down || button.nativePressed ? button.hoverColor : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            FocusRing {
                targetRadius: parent.height / 2
                visible: button.visualFocus
            }
        }

        HoverHandler {
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
        }
    }

    WindowButton {
        iconName: "minus"
        tooltip: qsTr("Minimize")
        onClicked: root.minimizeRequested()
    }

    WindowButton {
        id: maximizeButton
        objectName: "windowMaximizeButton"
        property bool snapEnabled: root.snapEnabled
        iconName: root.maximized ? "copy" : "square"
        tooltip: root.maximized ? qsTr("Restore") : qsTr("Maximize")
        onClicked: root.maximizeRequested()
        // Windows 11 Snap Layouts: the system asks where the maximize button is (WindowChromeFilter reads this item's
        // place when it is asked, so layout and DPI changes need no update here).
        // (The filter holds the item weakly: a destroyed one drops out by itself.)
        Component.onCompleted: AppShell.registerMaximizeButton(maximizeButton)
    }

    WindowButton {
        iconName: "x"
        tooltip: qsTr("Close")
        hoverColor: Theme.danger
        hoverIconColor: Theme.scrimText
        onClicked: root.closeRequested()
    }
}
