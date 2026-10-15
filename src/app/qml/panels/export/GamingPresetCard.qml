pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// One export preset as a tile of the Export panel's grid (design language v3; M5o's gaming card, and through
// PresetCard every built-in preset): the destination's glyph in a circle of its hue, the name, what it makes (two lines
// at most), and the estimate for the current range - the size as a mono pill and the export time beside a timer
// ("≈ 142 MB  ⏱ 1:05"; `estimate` keeps the whole line for screen readers and tests). Selected = the accent tile with
// black text and a black glyph well. A preset the exporter cannot make yet is disabled with a "Coming soon" tag and
// says why. Space/Enter select (radio semantics); GamingPresets makes its grid one Tab stop with arrow moves.
// Laid out with anchors, not Layouts: the tile's parts follow its width at once, so nothing outgrows the tile.
AbstractButton {
    id: control

    property string iconName: ""
    property string description: ""
    property bool selected: false
    property bool available: true
    /// The destination's colour (the glyph circle).
    property color hue: Theme.accentStroke
    /// Why it is unavailable ("Coming soon: ...").
    property string reason: ""
    /// The estimate line ("≈ 142 MB · about 1 min 5 s"), or the reason the range does not fit ("" while unknown).
    property string estimate: ""
    property bool estimateIsError: false
    /// The estimate's parts for the tile ("142 MB", 65 s); unset = the whole `estimate` line in the pill.
    property string sizeText: ""
    property real encodeSeconds: -1

    readonly property color ink: selected ? Theme.accentOnFill : Theme.textPrimary
    readonly property color subInk: selected ? Qt.alpha(Theme.accentOnFill, 0.72) : Theme.textSecondary
    readonly property bool split: !estimateIsError && sizeText !== ""

    /// "0:42", "1:05", "1:02:10".
    function clock(seconds) {
        const total = Math.max(1, Math.round(seconds))
        const h = Math.floor(total / 3600)
        const m = Math.floor(total / 60) % 60
        const s = total % 60
        const two = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s)
    }

    implicitHeight: Math.max(Theme.space(22), implicitContentHeight + topPadding + bottomPadding)
    implicitWidth: Theme.space(44)
    padding: Theme.space(2.5)
    enabled: available
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.RadioButton
    Accessible.name: control.text
    Accessible.description: control.available ? control.description + ". " + control.estimate : control.reason
    Accessible.checkable: true
    Accessible.checked: control.selected

    scale: control.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    contentItem: Item {
        implicitWidth: glyph.width + Theme.space(2.5) + Theme.space(30)
        implicitHeight: Math.max(glyph.height, nameText.height + Theme.space(0.5) + descriptionText.height
                                               + (footRow.visible ? Theme.space(2) + footRow.height : 0))
        opacity: control.available ? 1 : Theme.disabledOpacity

        // The destination's glyph: its hue (black glyph); on the selected tile a black well with the accent glyph.
        Rectangle {
            id: glyph
            width: Theme.space(7)
            height: Theme.space(7)
            radius: height / 2
            color: control.selected ? Theme.accentOnFill : control.available ? control.hue : Theme.well
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            Icon {
                anchors.centerIn: parent
                name: control.iconName
                size: Theme.iconSm
                color: control.selected ? Theme.accentFill : control.available ? Theme.clipInk : Theme.textSecondary
            }
        }

        Text {
            id: nameText
            anchors.left: glyph.right
            anchors.leftMargin: Theme.space(2.5)
            anchors.right: parent.right
            anchors.top: parent.top
            text: control.text
            elide: Text.ElideRight
            color: control.ink
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Theme.weightTitle
        }
        Text {
            id: descriptionText
            anchors.left: nameText.left
            anchors.right: parent.right
            anchors.top: nameText.bottom
            anchors.topMargin: Theme.space(0.5)
            text: control.available ? control.description : control.reason
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: control.subInk
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Theme.weightBody
        }

        // The estimate (size pill and export time), why the range does not fit, or "Coming soon"; at the tile's foot.
        Item {
            id: footRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Theme.space(5.5)
            visible: !control.available || control.estimate !== ""

            Rectangle {
                id: estimatePill
                width: Math.min(footRow.width, (control.available ? estimateText.implicitWidth : soonText.implicitWidth)
                                               + Theme.space(4))
                height: parent.height
                radius: height / 2
                color: control.selected ? Qt.alpha(Theme.accentOnFill, 0.14)
                     : control.available ? Theme.sunken : "transparent"
                border.width: control.available ? 0 : Theme.hairline
                border.color: Theme.borderStrong

                Text {
                    id: estimateText
                    objectName: "gamingPresetEstimate"
                    visible: control.available
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(2)
                    verticalAlignment: Text.AlignVCenter
                    text: control.split ? qsTr("≈ %1").arg(control.sizeText) : control.estimate
                    elide: Text.ElideRight
                    color: control.estimateIsError ? (control.selected ? Theme.accentOnFill : Theme.warning)
                                                   : control.ink
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Bold
                }
                Text {
                    id: soonText
                    visible: !control.available
                    anchors.centerIn: parent
                    text: qsTr("Coming soon")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Theme.weightLabel
                }
            }
            // The export time.
            Icon {
                id: timerIcon
                visible: control.available && control.split && control.encodeSeconds >= 0
                         && estimatePill.width + Theme.space(12) <= footRow.width
                anchors.left: estimatePill.right
                anchors.leftMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                name: "timer"
                size: Theme.iconXs
                color: control.subInk
            }
            Text {
                visible: timerIcon.visible
                anchors.left: timerIcon.right
                anchors.leftMargin: Theme.space(1)
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: control.encodeSeconds >= 0 ? control.clock(control.encodeSeconds) : ""
                elide: Text.ElideRight
                color: control.subInk
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusCard
        color: control.selected ? Theme.accentFill
             : control.hovered && control.available ? Theme.hover : Theme.section
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: Theme.radiusCard
            visible: control.visualFocus
        }
    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }
}
