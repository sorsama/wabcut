pragma ComponentBehavior: Bound
import QtQuick

// ● A CAPTION PRESET'S LOOK, MOVING (card 074): the picture inside a preset's tile in the Caption Style gallery. Three
// sample words in the preset's colours, weight, outline and box; while the tile is hovered or has the keyboard focus the
// words are "said" one after the other and the preset's animation plays on them the way the program monitor plays it -
// each word popping in, hopping, taking the highlight bar, typing itself, growing; an emphasised word in the emphasis
// colour. Resting (and with Theme.reduceMotion) the middle word is the one being said and nothing moves. A sketch of
// the look, not a render: the monitor draws the real caption. `preset` is a row of Captions.presets.
Item {
    id: tile
    Accessible.ignored: true

    property var preset: ({})
    /// Plays the animation (hover, keyboard focus).
    property bool animate: false

    readonly property string anim: tile.preset.animation ?? "none"
    readonly property real speed: Math.max(0.05, tile.preset.animationSpeed ?? 0.15)
    readonly property bool overshoot: (tile.preset.animationEase ?? "overshoot") === "overshoot"
    readonly property bool playing: tile.animate && !Theme.reduceMotion
    /// The word being said: the middle one at rest.
    property int cursor: 1
    /// 0..1: how much of the said word a typewriter has typed.
    property real typed: 1
    readonly property var words: {
        const sample = qsTr("Say it loud").split(" ")
        const c = tile.preset.textCase ?? "asWritten"
        return sample.map(w => c === "upper" ? w.toUpperCase() : c === "lower" ? w.toLowerCase() : w)
    }
    readonly property bool bar: tile.anim === "highlightBar"
    readonly property bool byWord: tile.anim === "wordByWord" || tile.anim === "wordPop"
    readonly property bool boxed: tile.preset.box === true

    Timer {
        running: tile.playing
        interval: 700
        repeat: true
        onTriggered: tile.cursor = (tile.cursor + 1) % 3
    }
    onPlayingChanged: {
        if (!tile.playing)
            tile.cursor = 1
    }
    onCursorChanged: {
        tile.typed = tile.anim === "typewriter" && tile.playing ? 0 : 1
        typing.restart()
        // The whole-caption animations replay at the first word.
        if (tile.cursor === 0 && tile.playing && (tile.anim === "pop" || tile.anim === "fade"))
            wholeIn.restart()
    }
    NumberAnimation {
        id: typing
        target: tile
        property: "typed"
        to: 1
        duration: tile.playing && tile.anim === "typewriter" ? 450 : 0
    }

    // The block: an optional box behind the three words.
    Rectangle {
        id: block
        anchors.centerIn: parent
        width: Math.min(tile.width - 6, row.implicitWidth + 14)
        height: row.implicitHeight + 10
        radius: Theme.radiusBadge
        visible: tile.boxed
        color: tile.preset.boxColor ?? Theme.videoBlack
        opacity: 0.85
    }
    // Pop and fade play on the whole caption.
    property real wholeScale: 1
    property real wholeOpacity: 1
    ParallelAnimation {
        id: wholeIn
        NumberAnimation { target: tile; property: "wholeScale"; from: 0.8; to: 1; duration: tile.speed * 1000
                          easing.type: tile.overshoot ? Easing.OutBack : Easing.OutCubic }
        NumberAnimation { target: tile; property: "wholeOpacity"; from: 0; to: 1; duration: tile.speed * 1000 }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space(1.5)
        scale: tile.playing && (tile.anim === "pop") ? tile.wholeScale : 1
        opacity: tile.playing && (tile.anim === "fade" || tile.anim === "pop") ? tile.wholeOpacity : 1

        Repeater {
            model: tile.words
            delegate: Item {
                id: word
                required property string modelData
                required property int index
                readonly property bool said: word.index === tile.cursor
                readonly property bool shown: !tile.byWord || word.index <= tile.cursor
                // The third sample word is the emphasised one when the preset emphasises words.
                readonly property bool emphasised: (tile.preset.emphasis ?? "none") !== "none" && word.index === 2
                readonly property bool onBar: tile.bar && word.said
                readonly property string letters: tile.anim === "typewriter" && word.said
                                                  ? word.modelData.slice(0, Math.ceil(tile.typed * word.modelData.length))
                                                  : word.modelData
                // The preset's per-word motion: pop in from half size, hop, grow; each settles back when its word is done.
                property real popScale: 1
                property real hop: 0
                property real zoomScale: tile.anim === "zoom" && word.said ? 1.25 : 1
                Behavior on zoomScale {
                    enabled: tile.playing
                    NumberAnimation { duration: tile.speed * 1000; easing.type: tile.overshoot ? Easing.OutBack : Easing.OutCubic }
                }
                implicitWidth: label.implicitWidth
                implicitHeight: label.implicitHeight
                opacity: word.shown ? 1 : 0
                scale: word.popScale * word.zoomScale * (word.emphasised ? (tile.preset.emphasisScale ?? 1.12) : 1)
                y: -word.hop

                onSaidChanged: {
                    if (!word.said || !tile.playing)
                        return
                    if (tile.anim === "wordPop")
                        popIn.restart()
                    else if (tile.anim === "bounce")
                        hopUp.restart()
                }
                NumberAnimation {
                    id: popIn
                    target: word
                    property: "popScale"
                    from: 0.5
                    to: 1
                    duration: tile.speed * 1000
                    easing.type: tile.overshoot ? Easing.OutBack : Easing.OutCubic
                }
                SequentialAnimation {
                    id: hopUp
                    NumberAnimation { target: word; property: "hop"; from: 0; to: Theme.space(2); duration: tile.speed * 1000; easing.type: Easing.OutCubic }
                    NumberAnimation { target: word; property: "hop"; to: 0; duration: tile.speed * 1000; easing.type: Easing.InQuad }
                }

                Rectangle { // the highlight bar behind the word being said
                    anchors.fill: parent
                    anchors.margins: -2
                    anchors.topMargin: 0
                    anchors.bottomMargin: 0
                    radius: Theme.radiusBadge
                    color: tile.preset.highlight ?? Theme.accentFill
                    visible: word.onBar
                }
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: word.letters
                    color: word.emphasised ? (tile.preset.emphasisColor ?? Theme.accentFill)
                         : word.onBar ? Theme.accentOnFill
                         : word.said && (tile.anim === "wordPop" || tile.anim === "bounce" || tile.anim === "zoom")
                           ? (tile.preset.highlight ?? Theme.accentFill)
                         : (tile.preset.fill ?? "white")
                    style: (tile.preset.outlineWidth ?? 0) > 0 ? Text.Outline : Text.Normal
                    styleColor: Theme.accentOnFill
                    font.family: (tile.preset.fontFamily ?? "") !== "" ? tile.preset.fontFamily : Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: (tile.preset.fontWeight ?? 600) >= 800 ? Font.Black
                               : (tile.preset.fontWeight ?? 600) >= 700 ? Font.Bold : Font.DemiBold
                }
            }
        }
    }
}
