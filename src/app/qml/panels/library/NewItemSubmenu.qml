pragma ComponentBehavior: Bound
import QtQuick

// New Item (P.2, docs/motion.md "Bars and Tone and the counting leader"): Premiere's File > New / the Project panel's
// New Item menu for synthetic items - HD Bars and Tone (SMPTE RP 219), Bars and Tone (SMPTE EG 1), Black Video, Color
// Matte, Universal Counting Leader, Transparent Video - made for the current sequence's frame size and rate
// (media.newSynthetic, one undo step). Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not
// shadow ours.
ContextMenu {
    id: menu
    objectName: "libraryNewItemMenu"

    /// Why the last item could not be made ("" after a good one).
    property string error: ""

    function make(kind, params) {
        const result = CommandBridge.apply("media.newSynthetic", { kind: kind, params: params })
        menu.error = result.ok ? "" : (result.error ?? "")
    }

    title: qsTr("New item")

    ContextMenuItem {
        objectName: "newHdBarsItem"
        text: qsTr("HD Bars and Tone")
        onTriggered: menu.make("barsAndTone", { pattern: 0 })
    }
    ContextMenuItem {
        objectName: "newBarsItem"
        text: qsTr("Bars and Tone")
        onTriggered: menu.make("barsAndTone", { pattern: 1 })
    }
    ContextMenuItem {
        objectName: "newBlackVideoItem"
        text: qsTr("Black Video")
        onTriggered: menu.make("blackVideo", {})
    }
    ContextMenuItem {
        objectName: "newColorMatteItem"
        text: qsTr("Color Matte")
        onTriggered: menu.make("colorMatte", {})
    }
    ContextMenuItem {
        objectName: "newCountingLeaderItem"
        text: qsTr("Universal Counting Leader")
        onTriggered: menu.make("countingLeader", {})
    }
    ContextMenuItem {
        objectName: "newTransparentVideoItem"
        text: qsTr("Transparent Video")
        onTriggered: menu.make("transparentVideo", {})
    }
}
