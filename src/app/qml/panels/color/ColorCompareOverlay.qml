import QtQuick

// Over the program picture (I.5): the before/after split's handle (drag it), the side-by-side labels, and the HSL
// eyedropper (a click picks the qualifier). Fills the picture's frame exactly, so x is a fraction of the canvas.
Item {
    id: root
    objectName: "colorCompareOverlay"

    // Before / After.
    Item {
        anchors.fill: parent
        visible: Color.compareMode === 1

        Rectangle {
            id: line
            x: Math.round(Color.splitPosition * root.width) - width / 2
            width: 2
            height: parent.height
            color: Theme.scrimText
            opacity: 0.85
        }
        Rectangle {
            x: line.x + line.width / 2 - width / 2
            y: parent.height / 2 - height / 2
            width: 22
            height: 22
            radius: 11
            color: Theme.scrim
            border.width: Theme.hairline
            border.color: Theme.scrimText
            Icon {
                anchors.centerIn: parent
                name: "move-horizontal"
                size: Theme.iconXs
                color: Theme.scrimText
            }
        }
        Text {
            x: Theme.space(2)
            y: Theme.space(2)
            text: qsTr("Before")
            color: Theme.scrimText
            style: Text.Outline
            styleColor: Theme.scrim
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.letterSpacing: Theme.labelTracking
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(2)
            y: Theme.space(2)
            text: qsTr("After")
            color: Theme.scrimText
            style: Text.Outline
            styleColor: Theme.scrim
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.letterSpacing: Theme.labelTracking
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SplitHCursor
            enabled: !Color.eyedropper
            onPressed: mouse => Color.splitPosition = mouse.x / root.width
            onPositionChanged: mouse => {
                if (pressed)
                    Color.splitPosition = Math.max(0, Math.min(1, mouse.x / root.width))
            }
        }
    }

    // Reference side by side.
    Item {
        anchors.fill: parent
        visible: Color.compareMode === 2
        Text {
            x: Theme.space(2)
            y: Theme.space(2)
            text: qsTr("Reference %1").arg(Color.referenceLabel)
            color: Theme.scrimText
            style: Text.Outline
            styleColor: Theme.scrim
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.letterSpacing: Theme.labelTracking
        }
        Text {
            x: root.width / 2 + Theme.space(2)
            y: Theme.space(2)
            text: qsTr("Program")
            color: Theme.scrimText
            style: Text.Outline
            styleColor: Theme.scrim
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.letterSpacing: Theme.labelTracking
        }
    }

    // The eyedropper: in side-by-side mode the program is the right half.
    MouseArea {
        anchors.fill: parent
        visible: Color.eyedropper
        cursorShape: Qt.CrossCursor
        onClicked: mouse => {
            let nx = mouse.x / root.width
            let ny = mouse.y / root.height
            if (Color.compareMode === 2) {
                if (nx < 0.5)
                    return
                nx = (nx - 0.5) * 2
                ny = (ny - 0.25) * 2
            }
            Color.pickColor(nx, ny)
        }
    }
}
