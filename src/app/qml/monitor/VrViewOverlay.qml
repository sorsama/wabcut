pragma ComponentBehavior: Bound
import QtQuick

// 0.7 (VR / 360 video): the look-around surface of the VR viewer over a monitor's picture. While the viewer is on, the
// ProgramMonitor draws its equirectangular canvas as the flat view of someone in the middle of the sphere (vrView); this
// item takes the input: drag to look around (grab the scene and pull it), the wheel zooms the field of view, a double
// click (or Reset View, or Home) goes straight ahead again. Keyboard: the arrow keys turn the view, + and - zoom, Home
// resets. A small badge in the corner says where the view looks.
//
//   VrViewOverlay { anchors.fill: parent; monitor: programMonitor; visible: viewing }
Item {
    id: overlay
    objectName: "vrViewOverlay"

    /// The ProgramMonitor item whose view this controls.
    required property var monitor
    /// What the badge calls the picture ("Program" or "Source").
    property string subject: qsTr("Program")

    readonly property real degreesPerPixel: monitor.vrFov / Math.max(1, width)

    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("%1 monitor VR view. Drag to look around, scroll to zoom, double-click to reset.").arg(overlay.subject)
    Accessible.focusable: true

    function describeAngle(value: real): string {
        return qsTr("%1°").arg(Math.round(value))
    }

    // The keys below are the viewer's while it has focus: the arrow keys and Home are the app's shortcuts otherwise.
    function claims(key: int): bool {
        return key === Qt.Key_Left || key === Qt.Key_Right || key === Qt.Key_Up || key === Qt.Key_Down || key === Qt.Key_Plus
               || key === Qt.Key_Equal || key === Qt.Key_Minus || key === Qt.Key_Home
    }
    Keys.onShortcutOverride: event => {
        if (overlay.claims(event.key) && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0)
            event.accepted = true
    }
    Keys.onPressed: event => {
        const step = Math.max(1, monitor.vrFov / 12)
        if (event.key === Qt.Key_Left)
            monitor.vrLook(-step, 0)
        else if (event.key === Qt.Key_Right)
            monitor.vrLook(step, 0)
        else if (event.key === Qt.Key_Up)
            monitor.vrLook(0, step)
        else if (event.key === Qt.Key_Down)
            monitor.vrLook(0, -step)
        else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal)
            monitor.vrZoom(1.15)
        else if (event.key === Qt.Key_Minus)
            monitor.vrZoom(1 / 1.15)
        else if (event.key === Qt.Key_Home)
            monitor.vrReset()
        else
            return
        event.accepted = true
    }

    MouseArea {
        id: look
        objectName: "vrLookArea"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: look.pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property real lastX: 0
        property real lastY: 0

        onPressed: mouse => {
            overlay.forceActiveFocus(Qt.MouseFocusReason)
            look.lastX = mouse.x
            look.lastY = mouse.y
        }
        onPositionChanged: mouse => {
            if (!look.pressed)
                return
            // The scene follows the pointer: pulling it to the right turns the view to the left.
            overlay.monitor.vrLook(-(mouse.x - look.lastX) * overlay.degreesPerPixel,
                                   (mouse.y - look.lastY) * overlay.degreesPerPixel)
            look.lastX = mouse.x
            look.lastY = mouse.y
        }
        onDoubleClicked: overlay.monitor.vrReset()
        onWheel: wheel => {
            overlay.monitor.vrZoom(wheel.angleDelta.y > 0 ? 1.1 : 1 / 1.1)
            wheel.accepted = true
        }
    }

    // The badge and Reset View sit side by side and wrap onto two lines on a narrow monitor.
    Flow {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(2)
        spacing: Theme.space(2)

        Rectangle {
            id: badge
            objectName: "vrViewBadge"
            width: Math.min(bar.width, badgeRow.implicitWidth + Theme.space(5))
            height: Theme.controlHeightSmall
            radius: height / 2
            color: Theme.scrim
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("VR view: yaw %1, pitch %2, field of view %3")
                                 .arg(overlay.describeAngle(overlay.monitor.vrYaw))
                                 .arg(overlay.describeAngle(overlay.monitor.vrPitch))
                                 .arg(overlay.describeAngle(overlay.monitor.vrFov))

            Row {
                id: badgeRow
                anchors.centerIn: parent
                spacing: Theme.space(3)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("VR")
                    color: Theme.scrimText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, bar.width - Theme.space(12))
                    text: qsTr("Yaw %1  Pitch %2  FOV %3")
                              .arg(overlay.describeAngle(overlay.monitor.vrYaw))
                              .arg(overlay.describeAngle(overlay.monitor.vrPitch))
                              .arg(overlay.describeAngle(overlay.monitor.vrFov))
                    color: Theme.scrimTextMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
            }
        }

        PillButton {
            id: resetButton
            objectName: "vrResetView"
            compact: true
            text: qsTr("Reset View")
            tooltip: qsTr("Look straight ahead again (Home, or double-click the picture)")
            onClicked: overlay.monitor.vrReset()
        }
    }
}
