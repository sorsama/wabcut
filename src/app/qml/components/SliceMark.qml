import QtQuick

// The section mark (design language v3): a round dot of Theme.dotSize before section titles and
// as page dots. v2 drew a slanted bar here; the name stays for its callers. A box larger than the dot centres it.
Item {
    id: root

    property color color: Theme.accentStroke

    implicitWidth: Theme.dotSize
    implicitHeight: Theme.dotSize

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(root.width, root.height, Theme.dotSize)
        height: width
        radius: width / 2
        color: root.color
    }
}
