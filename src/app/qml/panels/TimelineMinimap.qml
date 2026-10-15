import QtQuick

// Overview of the whole sequence: TimelineOverview (C++, one batched mesh for every clip) plus the
// visible-range window. Drag the window to scroll; click elsewhere to center the view there.
Rectangle {
    id: root

    required property TimelineView view

    readonly property real contentWidth: Math.max(1, view.contentWidth)

    implicitHeight: Theme.minimapHeight
    // Design language v3: a pill well, the visible range a white-edged pill over it.
    radius: height / 2
    color: Theme.sunken
    clip: true

    Accessible.role: Accessible.ScrollBar
    Accessible.name: qsTr("Timeline overview")

    TimelineOverview {
        anchors.fill: parent
        anchors.topMargin: Theme.space(1)
        anchors.bottomMargin: Theme.space(1)
        view: root.view
        clipColors: Theme.clipColors
        clipOpacity: Theme.minimapClipOpacity
        rowGap: Theme.hairline
    }

    TapHandler {
        onTapped: eventPoint => {
            const x = eventPoint.position.x
            if (x < viewport.x || x > viewport.x + viewport.width)
                root.view.scrollX = x / root.width * root.contentWidth - root.view.width / 2
        }
    }

    Rectangle {
        id: viewport
        x: root.view.scrollX / root.contentWidth * root.width
        width: Math.max(Theme.space(3), root.view.width / root.contentWidth * root.width)
        height: root.height
        radius: height / 2
        color: Qt.alpha(Theme.textPrimary, 0.08)
        border.width: Theme.focusRingWidth
        border.color: Theme.textPrimary

        DragHandler {
            id: drag
            property real startScroll: 0
            target: null
            xAxis.enabled: true
            yAxis.enabled: false
            cursorShape: Qt.SizeHorCursor
            onActiveChanged: {
                if (active)
                    startScroll = root.view.scrollX
            }
            onActiveTranslationChanged: {
                if (active)
                    root.view.scrollX = startScroll + activeTranslation.x / root.width * root.contentWidth
            }
        }
    }
}
