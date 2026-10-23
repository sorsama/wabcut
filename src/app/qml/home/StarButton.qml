pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Shapes

// The star on a project card or row on Home (design language v3: lime and filled when starred). It stars the project in
// Preferences ("home/starred"); starred projects stay in the recent list past its limit and show under Starred.
AbstractButton {
    id: star

    required property string path
    required property string projectName
    // Read again whenever the starred list changes (Preferences.homeStarred).
    readonly property bool starred: Preferences.homeStarred !== "" && Preferences.isStarred(path)

    objectName: "homeRecentStar"
    implicitWidth: Theme.controlHeight
    implicitHeight: Theme.controlHeight
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: starred ? qsTr("Unstar %1").arg(projectName) : qsTr("Star %1").arg(projectName)
    Accessible.checkable: true
    Accessible.checked: starred
    onClicked: Preferences.setStarred(star.path, !star.starred)

    background: Rectangle {
        radius: height / 2
        color: star.starred ? Theme.brandLime : star.hovered ? Theme.hover : Theme.raised
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }
    contentItem: Item {
        Shape {
            id: glyph
            anchors.centerIn: parent
            width: Theme.iconXs
            height: Theme.iconXs
            preferredRendererType: Shape.CurveRenderer
            readonly property real u: width / 24
            ShapePath {
                strokeColor: star.starred ? Theme.clipInk : Theme.textSecondary
                strokeWidth: 2 * glyph.u
                joinStyle: ShapePath.RoundJoin
                fillColor: star.starred ? Theme.clipInk : "transparent"
                PathPolyline {
                    path: [Qt.point(12, 2), Qt.point(15.1, 8.3), Qt.point(22, 9.3), Qt.point(17, 14.1),
                           Qt.point(18.2, 21), Qt.point(12, 17.8), Qt.point(5.8, 21), Qt.point(7, 14.1),
                           Qt.point(2, 9.3), Qt.point(8.9, 8.3), Qt.point(12, 2)]
                           .map(p => Qt.point(p.x * glyph.u, p.y * glyph.u))
                }
            }
        }
    }
    Loader {
        active: star.hovered
        sourceComponent: Tooltip {
            parent: star
            visible: true
            text: star.starred ? qsTr("Unstar") : qsTr("Star: keep it under Starred")
        }
    }
    FocusRing {
        targetRadius: star.height / 2
        visible: star.visualFocus
    }
    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
