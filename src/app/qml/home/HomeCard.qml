import QtQuick
import QtQuick.Layouts

// A card on the Home screen (design language v3): an island (the panel surface on the black stage, radius 22, no edge,
// no slice) headed by a small caps label ("CONTINUE", "RECENT"), optionally after a hue dot, with tools at the right of
// that row, then its content. Home's pages give their islands a fixed height; the content fills it.
// When the Home screen opens (`revealed`), cards rise 12 px and fade in, one after another (`order`), with the soft
// ease-out; under reduce-motion they simply appear.
Item {
    id: root

    property string title: ""
    /// A hue dot before the title (a Theme.hue name, e.g. "mint"); "" for none.
    property string hue: ""
    property int order: 0
    property bool revealed: true
    property int padding: Theme.space(4)
    default property alias contentData: body.data
    property alias trailing: trailingRow.data

    implicitHeight: column.implicitHeight + padding * 2
    opacity: revealed ? 1 : 0
    Behavior on opacity {
        SequentialAnimation {
            PauseAnimation { duration: root.revealed ? Theme.dur(root.order * 60) : 0 }
            NumberAnimation { duration: Theme.durCollapse; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
        }
    }
    transform: Translate {
        y: root.revealed ? 0 : Theme.space(3)
        Behavior on y {
            SequentialAnimation {
                PauseAnimation { duration: root.revealed ? Theme.dur(root.order * 60) : 0 }
                NumberAnimation { duration: Theme.durSpring; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusPanel
        color: Theme.panel
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: Theme.space(3)

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.minimumHeight: Theme.controlHeightSmall
            visible: root.title !== ""
            spacing: Theme.space(2)

            Rectangle {
                visible: root.hue !== ""
                implicitWidth: Theme.dotSize
                implicitHeight: Theme.dotSize
                radius: Theme.dotSize / 2
                color: Theme.hue(root.hue)
            }
            Text {
                Layout.fillWidth: true
                text: root.title
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking * 1.5
            }
            Row {
                id: trailingRow
                spacing: Theme.space(1)
            }
        }

        // Fills the card when it is given more than its content's height (Home's pages): a child with
        // Layout.fillHeight takes the room (a list that scrolls inside).
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.space(3)
        }
    }
}
