pragma ComponentBehavior: Bound
import QtQuick

// Highlights > Reel options (M5o, gaming): how highlights.toReel strings the accepted moments together: in time or
// score order, how long at most, the transition, music from the Library (cuts land on its beats) and slow motion at
// each peak. In its own file, importing only QtQuick, so the Wabcut ContextMenu (the menus) is not shadowed.
Popover {
    id: root
    objectName: "highlightReelOptions"

    title: qsTr("Reel options")
    transformOrigin: Item.BottomRight

    readonly property var lengths: [0, 60, 90, 180]
    readonly property var transitions: [{ id: "crossDissolve", name: qsTr("Cross dissolve") },
                                        { id: "dipToBlack", name: qsTr("Dip to black") },
                                        { id: "dipToWhite", name: qsTr("Dip to white") },
                                        { id: "none", name: qsTr("Cut (no transition)") }]
    property string transition: "crossDissolve"
    property string music: ""
    property string musicName: ""

    /// highlights.toReel's options, for Highlights.makeReel().
    function options() {
        return { order: orderTabs.currentIndex === 1 ? "score" : "time", maxDuration: root.lengths[lengthTabs.currentIndex],
                 transition: root.transition, music: root.music, slowMotion: slowToggle.checked }
    }
    function transitionName(): string {
        const t = root.transitions.find(t => t.id === root.transition)
        return t ? t.name : ""
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(72)
        spacing: Theme.space(2)

        GroupLabel { text: qsTr("Order") }
        PillTabs {
            id: orderTabs
            objectName: "highlightsReelOrder"
            width: parent.width
            compact: true
            model: [{ label: qsTr("As it happened") }, { label: qsTr("Best first") }]
            currentIndex: 0
            Accessible.name: qsTr("Order")
        }

        GroupLabel { text: qsTr("Length") }
        PillTabs {
            id: lengthTabs
            objectName: "highlightsReelLength"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Any") }, { label: qsTr("1 min") }, { label: qsTr("90 s") }, { label: qsTr("3 min") }]
            currentIndex: 0
            Accessible.name: qsTr("Longest reel")
        }

        GroupLabel { text: qsTr("Transition") }
        PillButton {
            id: transitionButton
            objectName: "highlightsReelTransition"
            width: parent.width
            compact: true
            iconName: "chevron-down"
            text: root.transitionName()
            tooltip: qsTr("Transition between moments")
            Accessible.name: qsTr("Transition: %1").arg(root.transitionName())
            onClicked: transitionMenu.popup(transitionButton, 0, transitionButton.height + Theme.space(1))
            OptionsMenu {
                id: transitionMenu
                objectName: "highlightsReelTransitionMenu"
                options: root.transitions
                current: root.transition
                onChosen: id => root.transition = id
            }
        }

        GroupLabel { text: qsTr("Music") }
        PillButton {
            id: musicButton
            objectName: "highlightsReelMusic"
            width: parent.width
            compact: true
            iconName: "music"
            text: root.music !== "" ? root.musicName : qsTr("No music")
            tooltip: qsTr("Music from the Library: the cuts land on its beats")
            Accessible.name: qsTr("Music: %1").arg(text)
            onClicked: {
                musicMenu.options = [{ id: "", name: qsTr("No music") }].concat(Highlights.musicChoices())
                musicMenu.popup(musicButton, 0, musicButton.height + Theme.space(1))
            }
            OptionsMenu {
                id: musicMenu
                objectName: "highlightsReelMusicMenu"
                current: root.music
                onChosen: id => {
                    root.music = id
                    const m = musicMenu.options.find(o => o.id === id)
                    root.musicName = m ? m.name : ""
                }
            }
        }

        Toggle {
            id: slowToggle
            objectName: "highlightsReelSlowMotion"
            text: qsTr("Slow motion at each peak")
            tooltip: qsTr("Half speed for a moment and a half around the peak")
        }
    }
}
