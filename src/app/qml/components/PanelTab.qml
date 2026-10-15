pragma ComponentBehavior: Bound
import QtQuick

// One tab of a panel's tab strip (design language v3): the active tab rises from the strip in the panel's own colour,
// its top corners rounded and its foot flared into the panel (two concave corners), with the panel's hue dot and its
// name; an inactive tab is just its dot and name on the strip. Panel draws a lone panel's tab; DockTabs draws a group's.
Item {
    id: root

    property string text: ""
    property color dotColor: Theme.accentStroke
    property bool active: true
    property bool hovered: false
    /// The panel's colour (the active tab and its flares).
    property color surfaceColor: Theme.panel
    /// Room kept at the right for a close button (DockTabs places it).
    property real trailingSpace: Theme.space(4)
    readonly property real naturalWidth: Math.ceil(label.implicitWidth) + Theme.dotSize + Theme.space(5) + root.trailingSpace

    implicitWidth: naturalWidth
    implicitHeight: Theme.tabHeight

    // The tab's body: rounded on top, square at its foot.
    Rectangle {
        anchors.fill: parent
        visible: root.active || root.hovered
        topLeftRadius: Theme.radiusTab
        topRightRadius: Theme.radiusTab
        color: root.active ? root.surfaceColor : Qt.alpha(root.surfaceColor, 0.55)
    }

    // The flares: the panel's colour curving out beside each foot (only the surface is drawn, so the strip - or a
    // hovered neighbour - shows through the curve).
    Repeater {
        model: root.active ? [-1, 1] : []

        delegate: TabFlare {
            required property int modelData
            side: modelData
            x: side < 0 ? -width : root.width
            y: root.height - height
            color: root.surfaceColor
        }
    }

    Row {
        id: content
        anchors.left: parent.left
        anchors.leftMargin: Theme.space(3)
        anchors.right: parent.right
        anchors.rightMargin: root.trailingSpace
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(2)

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.dotSize
            height: Theme.dotSize
            radius: width / 2
            color: root.dotColor
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, content.width - Theme.dotSize - content.spacing)
            text: root.text
            elide: Text.ElideRight
            color: root.active || root.hovered ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel

            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
    }
}
