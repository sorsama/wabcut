pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One highlight moment (M5o, gaming): its frame at the peak with the rank, its title, range and length, the score and
// confidence chip, a waveform of the range with the peak, the measured signals as bars (game audio, your voice, on
// screen, chat, a stream marker), up to three reasons (more on expand) and Accept / Reject / Jump / Preview. Accepted
// cards carry a success strip; rejected ones are dimmed (the panel lists them last). One column for the narrow
// browser slot.
Rectangle {
    id: card

    required property var modelData
    required property int index
    /// The list's keyboard position is on this card while the list has focus.
    property bool current: false
    property bool keyboardFocus: false
    property bool expanded: false
    readonly property int rank: modelData.rank
    readonly property string review: modelData.state
    readonly property var reasons: modelData.reasons ?? []
    readonly property var bars: modelData.signals ?? []
    readonly property var waveform: modelData.waveform ?? []
    readonly property bool previewing: Highlights.previewRank === rank

    signal acceptRequested()
    signal rejectRequested()
    signal jumpRequested()
    signal previewRequested()
    signal chosen()

    function signalColor(key: string): color {
        switch (key) {
        case "game": return Theme.accentFill
        case "mic": return Theme.info
        case "visual": return Theme.success
        case "chat": return Theme.markerColor(1)
        case "hint": return Theme.warning
        }
        return Theme.textTertiary
    }
    function confidenceText(): string {
        switch (card.modelData.confidenceLabel) {
        case "high": return qsTr("high")
        case "medium": return qsTr("medium")
        case "low": return qsTr("low")
        }
        return ""
    }

    objectName: "highlightCard_" + rank
    implicitHeight: column.implicitHeight + Theme.space(4)
    radius: Theme.radiusCard
    color: Theme.sunken
    border.width: card.current ? Theme.hairline * 2 : Theme.hairline
    border.color: card.current || hover.hovered ? Theme.accentStroke : Theme.border
    opacity: card.review === "rejected" ? 0.55 : 1
    Behavior on opacity { NumberAnimation { duration: Theme.durState } }
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Highlight %1: %2, %3, score %4%5").arg(rank).arg(modelData.title).arg(modelData.rangeText)
                         .arg(Math.round(modelData.score * 100))
                         .arg(review === "accepted" ? qsTr(", accepted") : review === "rejected" ? qsTr(", rejected") : "")

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: card.chosen() }

    FocusRing {
        anchors.margins: 0
        targetRadius: Theme.radiusCard
        visible: card.current && card.keyboardFocus
    }

    // Accepted: a success strip down the left edge.
    Rectangle {
        visible: card.review === "accepted"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(1)
        width: Theme.space(0.75)
        radius: width / 2
        color: Theme.success
    }

    component Chip: Rectangle {
        property alias text: chipLabel.text
        property alias textColor: chipLabel.color
        implicitWidth: chipLabel.implicitWidth + Theme.space(3)
        implicitHeight: chipLabel.implicitHeight + Theme.space(1)
        radius: Theme.radiusPill
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
        Text {
            id: chipLabel
            anchors.centerIn: parent
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(2)
        anchors.leftMargin: Theme.space(2.5)
        spacing: Theme.space(1.5)

        // The frame at the peak beside the title, range and score.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Rectangle {
                Layout.preferredWidth: Theme.space(26)
                Layout.preferredHeight: Theme.space(26) * 9 / 16
                Layout.alignment: Qt.AlignTop
                radius: Theme.radiusBadge
                color: Theme.canvas
                clip: true
                Image {
                    objectName: "highlightFrame"
                    anchors.fill: parent
                    source: card.modelData.image ?? ""
                    sourceSize: Qt.size(208, 117)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: Theme.space(1)
                    width: rankText.implicitWidth + Theme.space(2)
                    height: rankText.implicitHeight + Theme.space(0.5)
                    radius: Theme.radiusBadge
                    color: Theme.scrim
                    Text {
                        id: rankText
                        anchors.centerIn: parent
                        text: qsTr("#%1").arg(card.rank)
                        color: Theme.scrimText
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space(1)
                Text {
                    objectName: "highlightTitle"
                    Layout.fillWidth: true
                    text: card.modelData.title
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2").arg(card.modelData.rangeText).arg(card.modelData.durationText)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideRight
                }
                Chip {
                    objectName: "highlightScore"
                    text: card.confidenceText() !== ""
                          ? qsTr("Score %1 · %2 confidence").arg(Math.round(card.modelData.score * 100)).arg(card.confidenceText())
                          : qsTr("Score %1").arg(Math.round(card.modelData.score * 100))
                    textColor: card.modelData.confidenceLabel === "high" ? Theme.accentStroke : Theme.textSecondary
                }
            }
        }

        // The waveform of the range, the peak marked.
        Item {
            id: wave
            objectName: "highlightWaveform"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(5)
            visible: card.waveform.length > 0
            readonly property real cell: card.waveform.length > 0 ? width / card.waveform.length : 0
            Repeater {
                model: card.waveform
                delegate: Rectangle {
                    required property real modelData
                    required property int index
                    x: index * wave.cell + wave.cell * 0.15
                    width: Math.max(1, wave.cell * 0.7)
                    height: Math.max(Theme.hairline, modelData * wave.height)
                    y: (wave.height - height) / 2
                    radius: width / 2
                    color: Theme.accentStroke
                    opacity: 0.7
                }
            }
            Rectangle {
                readonly property real span: card.modelData.end - card.modelData.start
                x: span > 0 ? (card.modelData.peak - card.modelData.start) / span * (wave.width - width) : 0
                width: Theme.hairline * 2
                height: wave.height
                color: Theme.textPrimary
            }
        }

        // The measured signals.
        GridLayout {
            objectName: "highlightSignals"
            Layout.fillWidth: true
            visible: card.bars.length > 0
            columns: card.width >= Theme.space(80) ? 2 : 1
            columnSpacing: Theme.space(3)
            rowSpacing: Theme.space(0.5)
            Repeater {
                model: card.bars
                delegate: RowLayout {
                    id: bar
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    Text {
                        Layout.preferredWidth: Theme.space(17)
                        text: bar.modelData.label
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.space(1)
                        radius: height / 2
                        color: Theme.raised
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: qsTr("%1: %2 %").arg(bar.modelData.label).arg(Math.round(bar.modelData.value * 100))
                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, bar.modelData.value))
                            height: parent.height
                            radius: height / 2
                            color: card.signalColor(bar.modelData.key)
                        }
                    }
                }
            }
        }

        // Why it stood out: three reasons, the rest on expand.
        Repeater {
            model: card.expanded ? card.reasons : card.reasons.slice(0, 3)
            delegate: Text {
                required property string modelData
                Layout.fillWidth: true
                text: qsTr("• %1").arg(modelData)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
            }
        }
        PillButton {
            objectName: "highlightMoreReasons_" + card.rank
            visible: card.reasons.length > 3
            compact: true
            variant: "ghost"
            text: card.expanded ? qsTr("Fewer reasons") : qsTr("Show %1 more").arg(card.reasons.length - 3)
            onClicked: card.expanded = !card.expanded
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            IconButton {
                id: acceptButton
                objectName: "highlightAccept_" + card.rank
                compact: true
                iconName: "check"
                checkable: true
                checked: card.review === "accepted"
                checkedColor: Theme.success
                tooltip: checked ? qsTr("Accepted: click to undo (A)") : qsTr("Accept for the reel (A)")
                onClicked: {
                    checked = Qt.binding(() => card.review === "accepted")
                    card.acceptRequested()
                }
            }
            IconButton {
                objectName: "highlightReject_" + card.rank
                compact: true
                iconName: "x"
                checkable: true
                checked: card.review === "rejected"
                checkedColor: Theme.danger
                tooltip: checked ? qsTr("Rejected: click to undo (R)") : qsTr("Reject (R)")
                onClicked: {
                    checked = Qt.binding(() => card.review === "rejected")
                    card.rejectRequested()
                }
            }
            Item { Layout.fillWidth: true }
            IconButton {
                objectName: "highlightJump_" + card.rank
                compact: true
                iconName: "crosshair"
                enabled: card.modelData.onTimeline === true
                tooltip: qsTr("Go to the peak at %1 (Enter)").arg(card.modelData.peakText)
                onClicked: card.jumpRequested()
            }
            IconButton {
                objectName: "highlightPreview_" + card.rank
                compact: true
                iconName: card.previewing ? "pause" : "play"
                enabled: card.modelData.onTimeline === true
                checkable: true
                checked: card.previewing
                tooltip: card.previewing ? qsTr("Stop the preview (Space)") : qsTr("Play this moment (Space)")
                onClicked: {
                    checked = Qt.binding(() => card.previewing)
                    card.previewRequested()
                }
            }
        }
    }
}
