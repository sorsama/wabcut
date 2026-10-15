import QtQuick
import QtQuick.Controls.Basic

// Hairline between groups of ContextMenu items (inset from the menu's round edge).
MenuSeparator {
    topPadding: Theme.space(1)
    bottomPadding: Theme.space(1)
    leftPadding: Theme.space(3.5)
    rightPadding: Theme.space(3.5)

    contentItem: Rectangle {
        implicitWidth: Theme.space(40)
        implicitHeight: Theme.hairline
        color: Theme.border
        radius: height / 2
    }
}
