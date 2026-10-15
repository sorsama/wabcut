import QtQuick

// Safe margins over a monitor's picture (M5h, Premiere's "Safe Margins"): action safe (Theme.actionSafeInset per
// side) and title safe (Theme.titleSafeInset), with the centre cross. Fill the picture's frame with it; it takes no
// input. Shown while Preferences.safeMargins is on.
Item {
    id: root
    visible: Preferences.safeMargins
    enabled: false

    Rectangle {
        objectName: "actionSafe"
        anchors.fill: parent
        anchors.leftMargin: root.width * Theme.actionSafeInset
        anchors.rightMargin: root.width * Theme.actionSafeInset
        anchors.topMargin: root.height * Theme.actionSafeInset
        anchors.bottomMargin: root.height * Theme.actionSafeInset
        color: "transparent"
        border.width: Theme.hairline
        border.color: Theme.guide
    }
    Rectangle {
        objectName: "titleSafe"
        anchors.fill: parent
        anchors.leftMargin: root.width * Theme.titleSafeInset
        anchors.rightMargin: root.width * Theme.titleSafeInset
        anchors.topMargin: root.height * Theme.titleSafeInset
        anchors.bottomMargin: root.height * Theme.titleSafeInset
        color: "transparent"
        border.width: Theme.hairline
        border.color: Theme.guide
    }
    // Centre cross
    Rectangle {
        anchors.centerIn: parent
        width: Theme.space(4)
        height: Theme.hairline
        color: Theme.guide
    }
    Rectangle {
        anchors.centerIn: parent
        width: Theme.hairline
        height: Theme.space(4)
        color: Theme.guide
    }
}
