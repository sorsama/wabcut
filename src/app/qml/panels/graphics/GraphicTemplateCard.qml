pragma ComponentBehavior: Bound
import QtQuick

// A motion graphics template in the Essential Graphics browser: its live thumbnail (the graphic resolved and painted
// off the GUI thread, image://graphicthumb), name, length and where it lives. Drag it onto a track of the timeline
// (template.instantiate there), or double-click / Enter / Space to add it at the playhead.
FocusScope {
    id: card

    required property var modelData
    readonly property string key: modelData.key ?? ""

    signal addRequested(string key)
    signal menuRequested(string key, point at)

    objectName: "graphicTemplate_" + key
    implicitWidth: Theme.space(40)
    implicitHeight: thumb.height + nameText.implicitHeight + metaText.implicitHeight + Theme.space(3)
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1 template, %2 s").arg(modelData.name ?? "").arg(Number(modelData.seconds ?? 0).toFixed(1))
    Accessible.description: qsTr("Drag onto the timeline, or press Enter to add it at the playhead")
    Accessible.onPressAction: card.addRequested(card.key)
    // The card's keys are its own while it has focus (Space would otherwise play).
    Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) === 0
                                                       && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
    Keys.onSpacePressed: card.addRequested(card.key)
    Keys.onReturnPressed: card.addRequested(card.key)
    Keys.onEnterPressed: card.addRequested(card.key)

    Rectangle {
        id: thumb
        objectName: "graphicTemplateThumb"
        width: parent.width
        height: Math.round(width * Math.max(0.3, (card.modelData.height ?? 1080) / Math.max(1, card.modelData.width ?? 1920)))
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: card.activeFocus || hover.hovered ? Theme.focusRingWidth : Theme.hairline
        border.color: card.activeFocus || hover.hovered ? Theme.accentStroke : Theme.border
        clip: true

        Image {
            id: picture
            objectName: "graphicTemplateImage"
            anchors.fill: parent
            anchors.margins: Theme.hairline
            asynchronous: true
            cache: false
            fillMode: Image.PreserveAspectFit
            sourceSize: Qt.size(Math.max(1, Math.round(width)), Math.max(1, Math.round(height)))
            source: card.modelData.thumbnail ?? ""
        }
        Text {
            anchors.centerIn: parent
            visible: picture.status !== Image.Ready
            text: qsTr("Drawing…")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Rectangle {
            visible: (card.modelData.library ?? "") === "user"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: Theme.space(1)
            implicitWidth: libraryText.implicitWidth + Theme.space(2)
            implicitHeight: libraryText.implicitHeight + Theme.space(0.5)
            radius: Theme.radiusBadge
            color: Theme.scrim
            Text {
                id: libraryText
                anchors.centerIn: parent
                text: qsTr("Yours")
                color: Theme.scrimText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }
    }
    Text {
        id: nameText
        anchors.top: thumb.bottom
        anchors.topMargin: Theme.space(1)
        width: parent.width
        text: card.modelData.name ?? ""
        elide: Text.ElideRight
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    Text {
        id: metaText
        anchors.top: nameText.bottom
        width: parent.width
        readonly property int fieldCount: (card.modelData.fields ?? []).length
        readonly property string seconds: Number(card.modelData.seconds ?? 0).toFixed(1)
        text: fieldCount === 0 ? qsTr("%1 s").arg(seconds)
              : fieldCount === 1 ? qsTr("%1 s · 1 field").arg(seconds)
              : qsTr("%1 s · %n fields", "", fieldCount).arg(seconds)
        elide: Text.ElideRight
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    FocusRing {
        anchors.fill: thumb
        targetRadius: Theme.radiusField
        visible: card.activeFocus
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: card.forceActiveFocus(Qt.MouseFocusReason)
        onDoubleTapped: card.addRequested(card.key)
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => card.menuRequested(card.key, eventPoint.position)
    }
    // Drag onto the timeline: the template's key as application/x-wabcut-template (Main's timeline drop area).
    Item {
        id: dragProxy
        anchors.fill: parent
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: { "application/x-wabcut-template": card.key }
        DragHandler {
            target: null
            onActiveChanged: dragProxy.Drag.active = active
        }
    }
}
