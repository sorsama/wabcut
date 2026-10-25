pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtQuick.Controls.Basic

// One smart-search hit (AI-8): the matched frame, the source-range badge, the clip name and the transcript snippet
// with the matched words in bold. Drag it onto the timeline to edit in just that range ("application/x-wabcut-search-hit":
// overwrite, Ctrl/Cmd = insert, like library cards). 0.4: double-click, or Enter while it has focus, opens the clip in
// the Source Monitor with the hit's range marked (`opened`; SearchResultsPanel calls SourceMonitor.loadRange).
AbstractButton {
    id: card
    objectName: "searchHitCard"

    /// A SmartSearch.results entry: { mediaId, mediaName, rangeText, thumbnail, visual, transcript, snippet,
    /// snippetRich, hit, ... }
    required property var hit

    signal opened()

    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    padding: Theme.space(1.5)
    implicitHeight: Math.max(thumb.height, details.implicitHeight) + topPadding + bottomPadding

    //: Accessible name of a smart search result: clip, range and what was said
    Accessible.name: qsTr("%1, %2. %3").arg(hit.mediaName).arg(hit.rangeText).arg(hit.snippet)
    Accessible.description: qsTr("Enter opens it in the Source Monitor; drag to the timeline to insert this range")
    Accessible.onPressAction: card.opened()

    onDoubleClicked: card.opened()
    // Enter is the card's (not Render In to Out, not the Library's Next Row Field).
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
    Keys.onReturnPressed: event => {
        event.accepted = true
        card.opened()
    }
    Keys.onEnterPressed: event => {
        event.accepted = true
        card.opened()
    }

    background: Rectangle {
        radius: Theme.radiusField
        // The list gives the current card focus as Up / Down move (not a Tab focus reason): show it either way.
        color: card.hovered || card.activeFocus ? Theme.hover : "transparent"
        border.width: card.activeFocus ? Theme.focusRingWidth : 0
        border.color: Theme.accentStroke
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    contentItem: Item {
        implicitHeight: Math.max(thumb.height, details.implicitHeight)

        // The matched frame, clipped to the field radius.
        Rectangle {
            id: thumb
            objectName: "searchHitThumb"
            width: Math.round(Math.min(card.availableWidth * 0.42, 132))
            height: Math.round(width * 9 / 16)
            radius: Theme.radiusClip
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border

            Image {
                id: frame
                anchors.fill: parent
                source: card.hit.thumbnail
                sourceSize.height: Theme.mediaThumbnailHeight / 2
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                visible: false
            }
            Item {
                id: frameMask
                anchors.fill: parent
                layer.enabled: true
                visible: false
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusClip
                }
            }
            MultiEffect {
                anchors.fill: parent
                source: frame
                maskEnabled: true
                maskSource: frameMask
                visible: frame.status === Image.Ready
            }

            // Source range badge (bottom-right), like the library's duration badge.
            Rectangle {
                objectName: "searchHitRange"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(1)
                width: rangeText.implicitWidth + Theme.space(2)
                height: rangeText.implicitHeight + Theme.space(0.5)
                radius: Theme.radiusBadge
                color: Theme.scrim

                Text {
                    id: rangeText
                    anchors.centerIn: parent
                    text: card.hit.rangeText
                    color: Theme.scrimText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }

        Column {
            id: details
            anchors.left: thumb.right
            anchors.leftMargin: Theme.space(2)
            anchors.right: parent.right
            spacing: Theme.space(0.5)

            Row {
                width: parent.width
                spacing: Theme.space(1)

                Text {
                    width: parent.width - kinds.width - parent.spacing
                    text: card.hit.mediaName
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }
                // What matched: the frames (sparkles), the words (captions).
                Row {
                    id: kinds
                    spacing: Theme.space(0.5)
                    Icon {
                        visible: card.hit.visual
                        size: Theme.iconXs
                        name: "sparkles"
                        color: Theme.accentStroke
                    }
                    Icon {
                        visible: card.hit.transcript
                        size: Theme.iconXs
                        name: "captions"
                        color: Theme.textSecondary
                    }
                }
            }

            Text {
                width: parent.width
                visible: card.hit.snippet !== ""
                text: card.hit.snippetRich
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Text {
                width: parent.width
                visible: card.hit.snippet === ""
                //: Smart search result found by what the frames show (no matching words)
                text: qsTr("Matches what the picture shows")
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    HoverHandler {
        enabled: card.enabled
        cursorShape: Qt.PointingHandCursor
    }

    // Drag source for the timeline (the DragHandler takes over past the drag threshold; clicks still reach the card).
    Item {
        id: dragSource
        anchors.fill: parent
        Drag.active: dragHandler.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: { "application/x-wabcut-search-hit": card.hit.hit }
        Drag.imageSource: card.hit.thumbnail

        DragHandler {
            id: dragHandler
            target: null
        }
    }
}
