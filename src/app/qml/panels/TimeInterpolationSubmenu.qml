pragma ComponentBehavior: Bound
import QtQuick

// Time Interpolation (P.2, docs/motion.md): the clip menu's submenu, Premiere's Clip > Video Options > Time
// Interpolation. How the selected retimed video clips make the frames between their media's frames: Frame Sampling,
// Frame Blending, Optical Flow or (0.7) AI Frame Interpolation (clip.setTimeInterpolation on the selection, one undo step). The current choice of the
// clip under the menu is checked. Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not
// shadow ours.
ContextMenu {
    id: menu
    objectName: "timeInterpolationSubmenu"

    /// The clip's current method ("frameSampling", "frameBlending", "opticalFlow", "aiInterpolation"), read when the clip menu opens.
    property string current: "frameSampling"
    /// Whether the selection has a video clip to set it on.
    property bool hasVideo: false
    /// Why the last choice was refused ("" after a good one).
    property string error: ""

    function refresh() {
        const info = ProjectController.speedDurationInfo()
        menu.current = info.timeInterpolation ?? "frameSampling"
        menu.hasVideo = info.hasVideo ?? false
        menu.error = ""
    }
    function choose(method) {
        const result = CommandBridge.apply("clip.setTimeInterpolation", { method: method })
        menu.error = result.ok ? "" : (result.error ?? "")
        if (result.ok)
            menu.current = method
    }

    title: qsTr("Time Interpolation")

    ContextMenuItem {
        objectName: "frameSamplingItem"
        text: qsTr("Frame Sampling")
        checkable: true
        checked: menu.current === "frameSampling"
        enabled: menu.hasVideo
        onTriggered: menu.choose("frameSampling")
    }
    ContextMenuItem {
        objectName: "frameBlendingItem"
        text: qsTr("Frame Blending")
        checkable: true
        checked: menu.current === "frameBlending"
        enabled: menu.hasVideo
        onTriggered: menu.choose("frameBlending")
    }
    ContextMenuItem {
        objectName: "opticalFlowItem"
        text: qsTr("Optical Flow")
        checkable: true
        checked: menu.current === "opticalFlow"
        enabled: menu.hasVideo
        onTriggered: menu.choose("opticalFlow")
    }
    ContextMenuItem {
        objectName: "aiInterpolationItem"
        text: qsTr("AI Frame Interpolation")
        checkable: true
        checked: menu.current === "aiInterpolation"
        enabled: menu.hasVideo
        onTriggered: menu.choose("aiInterpolation")
    }
}
