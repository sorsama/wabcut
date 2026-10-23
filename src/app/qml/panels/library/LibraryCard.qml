pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtQuick.Controls.Basic

// A Library grid card (M5k): a bin (folder tile and item count; double-click opens it, drop items on it to move
// them), a sequence, or a media item: its thumbnail or poster frame (gradient in its label colour without a file), a
// 3px label strip, hover scrub over the timeline's filmstrip atlas (no decoding while the pointer moves), the duration
// (the timecode under the pointer while scrubbing), a subclip's range, and the proxy, offline and AI tags.
// Selected: an accent outline over a wash. Click selects (Ctrl/⌘ adds, Shift extends), double-click opens.
AbstractButton {
    id: card

    required property int index
    required property string key
    required property string kind
    required property string name
    required property int label
    required property string thumbnail
    required property string durationText
    required property string meta
    required property bool offline
    required property string proxyState
    required property real proxyProgress
    required property bool generated
    required property var subclip
    required property bool selected
    required property bool filmstrip
    required property int itemCount
    required property string binPath
    /// A picture arrived. A sequence the provider could not render answers with a 1 x 1 transparent image rather than
    /// an error (no warning per card): that is no picture, so the tint and the film icon stay.
    readonly property bool hasPicture: frame.status === Image.Ready && frame.implicitWidth > 1
    /// M5l: a stem's source ("separated from"), or "".
    required property string derivedFrom
    /// UX rework (stream T): the item's Source Monitor In/Out (Library.items `marks`: { in, out (0..1 of the card's
    /// range), inText, outText, durationText }, {} when unmarked): an accent band along the thumbnail's foot and the
    /// marked length in the badge. A drag onto the timeline edits in that range.
    property var marks: ({})
    readonly property bool marked: (card.marks.durationText ?? "") !== ""
    /// 0.2 (P.3): a search bin (its icon), and the Freeform view's metadata lines under the name (Freeform View
    /// Options; empty: the usual line).
    property bool searchBin: false
    property var metaLines: []

    /// Where the pointer last was across the thumbnail (0..1; poster frame "here").
    property real scrubFraction: 0
    readonly property bool scrubbing: Library.hoverScrub && card.filmstrip && thumbHover.hovered
    readonly property bool isBin: kind === "bin"
    readonly property bool isSequence: kind === "sequence"

    signal activated()
    signal menuRequested(point position)
    /// A drag of the card started, pressed at `position` in the card (the Freeform view moves it by the drop).
    signal dragStarted(point position)

    objectName: isSequence ? "librarySequence_" + key : "libraryCard_" + key
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    padding: 0
    Accessible.role: Accessible.ListItem
    Accessible.name: searchBin ? qsTr("Search bin %1, %n item(s) found", "", itemCount).arg(name)
                   : isBin ? qsTr("Bin %1, %n item(s)", "", itemCount).arg(name)
                           : qsTr("%1, %2").arg(name).arg(durationText !== "" ? durationText : meta)
    Accessible.selected: selected

    onDoubleClicked: card.activated()
    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
    Keys.onReturnPressed: card.activated()
    Keys.onEnterPressed: card.activated()

    contentItem: Item {
        Rectangle {
            id: thumb
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.round(width * 9 / 16)
            radius: Theme.radiusCard
            clip: true
            color: card.isBin ? Theme.sunken : Theme.raised
            border.width: card.selected ? Theme.clipSelectionWidth : Theme.hairline
            border.color: card.selected ? Theme.accentStroke : card.hovered ? Theme.borderStrong : Theme.border

            // Media without a picture: a dark body tinted with its label (design language v2: label-strip clips).
            Rectangle {
                anchors.fill: parent
                anchors.margins: thumb.border.width
                radius: Theme.radiusCard - thumb.border.width
                visible: !card.isBin && !card.hasPicture
                color: Qt.alpha(Theme.clipColor(Math.max(0, card.label)), card.selected ? Theme.clipTintSelected : Theme.clipTint)
            }
            Image {
                id: frame
                anchors.fill: parent
                anchors.margins: thumb.border.width
                visible: false
                source: card.thumbnail
                sourceSize.height: Theme.mediaThumbnailHeight
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
            }
            Item {
                id: frameMask
                anchors.fill: frame
                layer.enabled: true
                visible: false
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusCard - thumb.border.width
                }
            }
            MultiEffect {
                objectName: "mediaCardFrame"
                anchors.fill: frame
                source: frame
                maskEnabled: true
                maskSource: frameMask
                visible: card.hasPicture && !card.scrubbing
            }
            // Hover scrub: the filmstrip tile under the pointer.
            FilmstripScrub {
                id: scrub
                objectName: "libraryScrub"
                anchors.fill: frame
                visible: card.scrubbing && ready
                media: card.scrubbing ? card.key : ""
                position: card.scrubFraction
            }
            // Bins: a folder (the count is the line under the name); sequences and titles: their kind.
            Icon {
                anchors.centerIn: parent
                visible: card.isBin || (card.isSequence && !card.hasPicture) || card.kind === "title" || (card.kind === "audio" && !card.hasPicture)
                size: Theme.iconXl
                name: card.searchBin ? "folder-search" : card.isBin ? "folder" : card.isSequence ? "film" : card.kind === "audio" ? "music" : "type"
                color: card.isBin ? Theme.accentStroke : Theme.textSecondary
            }
            // The label strip along the top edge.
            Rectangle {
                visible: !card.isBin && card.label >= 0
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: thumb.border.width
                height: Theme.space(0.75)
                radius: Theme.radiusBadge
                color: Theme.clipColor(Math.max(0, card.label))
            }
            // The scrub line.
            Rectangle {
                visible: card.scrubbing
                x: Math.round(card.scrubFraction * (thumb.width - width))
                width: Theme.playheadWidth
                height: parent.height
                color: Theme.scrimText
            }
            // UX rework (stream T): the marked In to Out along the foot of the thumbnail.
            Rectangle {
                objectName: "libraryCardMarks"
                visible: card.marked && !card.isBin
                x: Math.round(thumb.border.width + (card.marks.in ?? 0) * (thumb.width - 2 * thumb.border.width))
                width: Math.max(Theme.space(1), Math.round(((card.marks.out ?? 1) - (card.marks.in ?? 0))
                                                           * (thumb.width - 2 * thumb.border.width)))
                height: Theme.space(1)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: thumb.border.width
                radius: height / 2
                color: Theme.accentFill
            }
            // Duration (the marked length when In/Out are set), or the timecode under the pointer.
            Rectangle {
                visible: badgeText.text !== ""
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(1.5)
                width: badgeText.implicitWidth + Theme.space(3)
                height: badgeText.implicitHeight + Theme.space(1)
                radius: Theme.radiusClip
                color: Theme.scrim
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: card.scrubbing && scrub.timecode !== "" ? scrub.timecode
                          : card.marked ? card.marks.durationText : card.durationText
                    color: Theme.scrimText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
            // Tags: offline, AI, subclip, proxy.
            Row {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.space(1.5)
                anchors.topMargin: Theme.space(2)
                spacing: Theme.space(1)
                Rectangle {
                    objectName: "offlineBadge"
                    visible: card.offline
                    width: offlineText.implicitWidth + Theme.space(3)
                    height: offlineText.implicitHeight + Theme.space(1)
                    radius: Theme.radiusBadge
                    color: Theme.warning
                    Text {
                        id: offlineText
                        anchors.centerIn: parent
                        //: Badge on a library card whose media file cannot be found
                        text: qsTr("Offline")
                        color: Theme.canvas
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                    }
                }
                // M5l: separated from another item (Separate Stems).
                Rectangle {
                    objectName: "stemBadge"
                    visible: card.derivedFrom !== ""
                    width: stemText.implicitWidth + Theme.space(2)
                    height: stemText.implicitHeight + Theme.space(1)
                    radius: Theme.radiusBadge
                    color: Theme.scrim
                    Text {
                        id: stemText
                        anchors.centerIn: parent
                        //: Micro-tag on a Library card of a separated stem
                        text: qsTr("Stem")
                        color: Theme.scrimText
                        font.family: Theme.fontSans
                        font.capitalization: Font.AllUppercase
                        font.pixelSize: Theme.textMicro
                        font.letterSpacing: Theme.microTracking
                        font.weight: Font.DemiBold
                    }
                }
                AiBadge {
                    objectName: "generatedBadge"
                    visible: card.generated
                    //: Tag on a Library card of AI-generated media
                    text: qsTr("AI")
                }
                Rectangle {
                    objectName: "subclipBadge"
                    visible: card.kind === "subclip"
                    width: subclipRow.implicitWidth + Theme.space(2)
                    height: subclipRow.implicitHeight + Theme.space(1)
                    radius: Theme.radiusBadge
                    color: Theme.scrim
                    Row {
                        id: subclipRow
                        anchors.centerIn: parent
                        spacing: Theme.space(1)
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "scissors"
                            size: Theme.iconXs
                            color: Theme.scrimText
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            //: Micro-tag on a subclip card
                            text: qsTr("Subclip")
                            color: Theme.scrimText
                            font.family: Theme.fontSans
                            font.capitalization: Font.AllUppercase
                            font.pixelSize: Theme.textMicro
                            font.letterSpacing: Theme.microTracking
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
            Rectangle {
                id: proxyChip
                objectName: "proxyBadge"
                readonly property bool making: card.proxyState === "generating" || card.proxyState === "paused"
                visible: card.proxyState !== ""
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(1.5)
                anchors.topMargin: Theme.space(2)
                width: proxyText.implicitWidth + Theme.space(3)
                height: proxyText.implicitHeight + Theme.space(1)
                radius: height / 2
                clip: true
                color: card.proxyState === "failed" || card.proxyState === "stale" ? Theme.warning : Theme.scrim
                Item {
                    visible: proxyChip.making
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width * card.proxyProgress
                    clip: true
                    // The proxy made so far: a flat accent fill (design language v3), grey while paused.
                    Rectangle {
                        width: proxyChip.width
                        height: parent.height
                        color: card.proxyState === "paused" ? Theme.border : Qt.alpha(Theme.accentFill, 0.55)
                    }
                }

                Text {
                    id: proxyText
                    anchors.centerIn: parent
                    text: card.proxyState === "ready" ? qsTr("Proxy")
                          : card.proxyState === "generating" ? qsTr("Proxy %1%").arg(Math.round(card.proxyProgress * 100))
                          : card.proxyState === "paused" ? qsTr("Proxy paused")
                          : card.proxyState === "queued" ? qsTr("Proxy queued")
                          : card.proxyState === "stale" ? qsTr("Proxy out of date")
                          : card.proxyState === "failed" ? qsTr("Proxy failed") : ""
                    color: card.proxyState === "failed" || card.proxyState === "stale" ? Theme.canvas : Theme.scrimText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
            }

            HoverHandler {
                id: thumbHover
                cursorShape: card.filmstrip && Library.hoverScrub ? Qt.SizeHorCursor : Qt.PointingHandCursor
                onPointChanged: if (hovered) card.scrubFraction = Math.max(0, Math.min(1, point.position.x / thumb.width))
            }
            FocusRing {
                anchors.fill: parent
                targetRadius: Theme.radiusCard
                visible: card.visualFocus
            }
        }

        Text {
            id: title
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: thumb.bottom
            anchors.topMargin: Theme.space(1.5)
            text: card.name
            elide: Text.ElideRight
            color: card.offline ? Theme.warning : Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold
        }
        Text {
            id: metaLine
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: title.bottom
            anchors.topMargin: Theme.space(0.5)
            text: card.metaLines.length > 0 ? card.metaLines[0]
                  : card.kind === "subclip" && card.subclip.inText !== undefined
                  ? qsTr("%1 – %2").arg(card.subclip.inText).arg(card.subclip.outText)
                  : card.derivedFrom !== "" ? qsTr("From %1").arg(card.derivedFrom)
                  : card.binPath !== "" ? card.binPath : card.meta
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        // The Freeform view's second metadata line.
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: metaLine.bottom
            visible: card.metaLines.length > 1 && card.metaLines[1] !== ""
            text: card.metaLines.length > 1 ? card.metaLines[1] : ""
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }
    background: null

    HoverHandler {
        enabled: card.enabled
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        id: cardTap
        acceptedButtons: Qt.LeftButton
        onSingleTapped: {
            card.forceActiveFocus(Qt.MouseFocusReason)
            const mods = cardTap.point.modifiers
            Library.select(card.key, (mods & Qt.ShiftModifier) ? Library.Range
                                     : (mods & (Qt.ControlModifier | Qt.MetaModifier)) ? Library.Toggle : Library.Replace)
        }
        onDoubleTapped: card.activated()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => {
            if (!card.selected)
                Library.select(card.key, Library.Replace)
            card.menuRequested(eventPoint.position)
        }
    }
    // Drag: into a bin, or (media, and a sequence to nest it) onto the timeline. 2026-10-15: in a real Windows drag a
    // sequence carrying application/x-wabcut-library alone never lit a bin up; with its key as timeline data too (like
    // media) it goes into bins. (Bins dragged into bins still do not, with either data: an open issue.)
    Item {
        id: dragProxy
        objectName: "libraryCardDrag"
        anchors.fill: parent
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.MoveAction | Qt.CopyAction
        Drag.imageSource: card.thumbnail
        DragHandler {
            id: cardDrag
            target: null
            enabled: !card.offline
            // The drag's data is made when it starts, after the card is selected (no card binds the selection).
            onActiveChanged: {
                if (active) {
                    card.dragStarted(cardDrag.centroid.pressPosition)
                    if (!card.selected)
                        Library.select(card.key, Library.Replace)
                    const keys = Library.selection.join(",")
                    dragProxy.Drag.mimeData = card.isBin
                                              ? { "application/x-wabcut-library": keys }
                                              : { "application/x-wabcut-library": keys, "application/x-wabcut-media": card.key }
                }
                dragProxy.Drag.active = active
            }
        }
    }
    DropArea {
        anchors.fill: parent
        enabled: card.isBin
        keys: ["application/x-wabcut-library"]
        onDropped: drop => {
            Library.moveItems(drop.getDataAsString("application/x-wabcut-library").split(","), card.key)
            drop.accept(Qt.MoveAction)
        }
        Rectangle {
            anchors.fill: parent
            visible: parent.containsDrag
            radius: Theme.radiusCard
            color: Theme.accentWash
            border.width: Theme.selectionWidth
            border.color: Theme.accentStroke
        }
    }
}
