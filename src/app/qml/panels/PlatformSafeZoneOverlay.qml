pragma ComponentBehavior: Bound
import QtQuick

// Platform safe zones over the Program Monitor's picture (0.4, CapCut-style; Premiere has none): on a portrait sequence
// (frame height > width) it shades where YouTube Shorts, TikTok or Instagram Reels put their own UI over the video -
// the top bar, the right-hand action column and the bottom caption / description band - hatched on a light scrim
// with a two-tone edge (dark outside, light inside: it reads on bright and dark footage) and a small label ("Shorts UI"). Program Monitor wrench > Platform Safe Zones picks the app
// (Preferences.platformSafeZone: "off", "shorts", "tiktok", "reels"; off by default). Monitor only: never in exports or
// Export Frame. It takes no input (clicks reach the picture and the overlays below) and hides while the Trim view or the
// multi-camera view shows. Fill the picture's frame with it.
Item {
    id: root
    objectName: "platformSafeZone"

    /// The bands of each app, as fractions of the frame (approximate public guides; one table). The Shorts builder's
    /// caption line (Y 0.68) sits above every bottom band.
    readonly property var bands: ({
        shorts: { top: 0.08, bottom: 0.79, right: 0.86, rightTop: 0.45, label: qsTr("Shorts UI") },
        tiktok: { top: 0.07, bottom: 0.80, right: 0.86, rightTop: 0.40, label: qsTr("TikTok UI") },
        reels: { top: 0.10, bottom: 0.80, right: 0.86, rightTop: 0.50, label: qsTr("Reels UI") }
    })
    readonly property var band: bands[Preferences.platformSafeZone] ?? bands.shorts

    visible: Preferences.platformSafeZone !== "off" && ProjectController.frameHeight > ProjectController.frameWidth
             && !Trim.viewOpen && !Multicam.viewOn
    enabled: false

    // One covered band: a light scrim, 45° hatching and an edge. Every line is two-tone, a dark stroke under a light
    // one, so the band reads on bright pictures as well as on dark ones.
    component Band: Rectangle {
        color: Qt.alpha(Theme.scrim, 0.3)
        border.width: Theme.hairline
        border.color: Theme.scrim // the edge's dark half, outside
        clip: true

        Canvas {
            id: hatch
            anchors.fill: parent
            anchors.margins: Theme.hairline
            readonly property color line: Theme.guide
            readonly property color under: Qt.alpha(Theme.scrim, 0.35)
            onLineChanged: requestPaint()
            onUnderChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const step = Theme.space(2)
                ctx.beginPath()
                for (let x = -hatch.height; x < hatch.width; x += step) {
                    ctx.moveTo(x, hatch.height)
                    ctx.lineTo(x + hatch.height, 0)
                }
                ctx.strokeStyle = hatch.under
                ctx.lineWidth = 2
                ctx.stroke()
                ctx.strokeStyle = hatch.line
                ctx.lineWidth = 1
                ctx.stroke()
            }
        }
        // The edge's light half, just inside the dark one.
        Rectangle {
            anchors.fill: parent
            anchors.margins: Theme.hairline
            color: "transparent"
            border.width: Theme.hairline
            border.color: Theme.scrimTextMuted
        }
    }

    Band {
        objectName: "platformSafeTop"
        x: 0
        y: 0
        width: root.width
        height: Math.round(root.height * root.band.top)
    }
    Band {
        objectName: "platformSafeBottom"
        x: 0
        y: Math.round(root.height * root.band.bottom)
        width: Math.round(root.width * root.band.right)
        height: root.height - y
    }
    Band {
        objectName: "platformSafeRight"
        x: Math.round(root.width * root.band.right)
        y: Math.round(root.height * root.band.rightTop)
        width: root.width - x
        height: Math.round(root.height * root.band.bottom) - y
    }

    // Which app's UI the bands stand for, in the top bar.
    Rectangle {
        objectName: "platformSafeLabel"
        x: Theme.space(2)
        y: Math.max(Theme.space(1), (Math.round(root.height * root.band.top) - height) / 2)
        width: label.implicitWidth + Theme.space(3)
        height: label.implicitHeight + Theme.space(1)
        radius: height / 2
        color: Theme.scrim
        visible: root.width > width + Theme.space(4)

        Text {
            id: label
            anchors.centerIn: parent
            text: root.band.label
            color: Theme.scrimText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
        }
    }
}
