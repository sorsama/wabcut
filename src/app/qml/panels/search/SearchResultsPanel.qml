pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Smart search results in the Library (AI-8): the hits of SmartSearch.results, best first, as cards that drag onto
// the timeline with their source range; above them the indexing pill ("Indexing 3/12" while it works, "Indexed
// 12/12" after) and, when the image model is not downloaded yet, an offer to download it with its license. Without
// the model, transcripts are still searched. Nothing leaves the computer except the one-time model download.
Item {
    id: root

    // The Library narrows them by its Find filters (M5k).
    property var results: SmartSearch.results

    Column {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Theme.space(2)

        // Indexing progress / state.
        StatusPill {
            id: indexPill
            objectName: "smartSearchIndexPill"
            visible: SmartSearch.indexText !== ""
            width: Math.min(implicitWidth, parent.width)
            label: SmartSearch.indexText
            detail: SmartSearch.indexing ? Math.round(SmartSearch.indexProgress * 100) + "%" : ""
            dotColor: SmartSearch.indexing ? Theme.accentStroke : SmartSearch.upToDate ? Theme.success : Theme.warning
            tooltip: SmartSearch.indexDetail
            clip: true

            // Progress under the label while indexing.
            BeamProgress {
                objectName: "smartSearchIndexProgress"
                visible: SmartSearch.indexing
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: parent.radius / 2
                anchors.rightMargin: parent.radius / 2
                height: Theme.hairline * 2
                value: SmartSearch.indexProgress
                label: SmartSearch.indexText
            }
            TapHandler {
                // Click: index now (or stop indexing).
                onTapped: SmartSearch.indexing ? SmartSearch.cancelIndexing() : SmartSearch.ensureIndexed()
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }

        // The image model is not downloaded: transcripts only until it is. One line; "Download…" shows the license
        // card with Accept / Not now (the license is shown before anything is downloaded).
        Rectangle {
            id: offerCard
            objectName: "smartSearchModelOffer"
            property bool expanded: false
            visible: SmartSearch.modelDownloadable
            width: parent.width
            height: offer.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.border

            Column {
                id: offer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(2)

                Item {
                    width: parent.width
                    height: Math.max(offerText.implicitHeight, offerButton.implicitHeight)

                    Text {
                        id: offerText
                        anchors.left: parent.left
                        anchors.right: offerButton.left
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        //: Smart search: offer to download the on-device image model, e.g. "Search what clips show (412 MB model)"
                        text: SmartSearch.downloading
                              ? qsTr("Downloading the image model… %1%").arg(Math.round(SmartSearch.downloadProgress * 100))
                              : qsTr("Search what clips show (%1 model)").arg(SmartSearch.modelSize)
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        id: offerButton
                        objectName: "smartSearchDownload"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !offerCard.expanded || SmartSearch.downloading
                        compact: true
                        variant: SmartSearch.downloading ? "ghost" : "secondary"
                        iconName: SmartSearch.downloading ? "x" : "download"
                        text: SmartSearch.downloading ? qsTr("Stop") : qsTr("Download…")
                        onClicked: {
                            if (SmartSearch.downloading)
                                SmartSearch.cancelDownload()
                            else
                                offerCard.expanded = true
                        }
                    }
                }
                Text {
                    width: parent.width
                    visible: offerCard.expanded && !SmartSearch.downloading
                    text: SmartSearch.modelLicense
                    wrapMode: Text.Wrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
                Row {
                    visible: offerCard.expanded && !SmartSearch.downloading
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "smartSearchAccept"
                        compact: true
                        variant: "primary"
                        iconName: "download"
                        text: qsTr("Accept and download")
                        onClicked: {
                            offerCard.expanded = false
                            SmartSearch.downloadModel()
                        }
                    }
                    PillButton {
                        compact: true
                        variant: "ghost"
                        text: qsTr("Not now")
                        onClicked: offerCard.expanded = false
                    }
                }
            }
        }

        Text {
            objectName: "smartSearchMessage"
            width: parent.width
            visible: text !== ""
            text: SmartSearch.searching && root.results.length === 0 ? qsTr("Searching…") : SmartSearch.message
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    ListView {
        id: list
        objectName: "smartSearchResults"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: header.height > 0 ? Theme.space(2) : 0
        anchors.leftMargin: -Theme.space(1.5)
        anchors.rightMargin: -Theme.space(1.5)
        clip: true
        spacing: Theme.space(0.5)
        model: root.results
        boundsBehavior: Flickable.StopAtBounds
        // 0.4: Tab from the search field reaches the results; Up / Down move between the cards (the keymap's plain
        // Up / Down stay out while the list has focus) and the focused card follows the current one.
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: -1
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Smart search results")

        Keys.onShortcutOverride: event => event.accepted = (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                                                           && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        onActiveFocusChanged: {
            if (activeFocus && count > 0 && (currentIndex < 0 || !currentItem))
                currentIndex = 0
        }
        onCurrentItemChanged: {
            if (currentItem && activeFocus)
                currentItem.forceActiveFocus(Qt.TabFocusReason)
        }

        ScrollBar.vertical: ThinScrollBar {}

        // 0.4: double-click or Enter opens the hit in the Source Monitor with its range marked (`,` / `.` then edit
        // exactly that range); the Program Monitor keeps the sequence.
        delegate: SearchHitCard {
            required property var modelData
            required property int index
            width: list.width
            hit: modelData
            onOpened: SourceMonitor.loadRange(modelData.mediaId, modelData.sourceIn, modelData.sourceOut)
            onActiveFocusChanged: {
                if (activeFocus)
                    list.currentIndex = index
            }
        }
    }
}
