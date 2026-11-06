pragma ComponentBehavior: Bound
import QtQuick

// The menu a clip's fx badge opens (0.7, card 041; TimelineView.fxBadgeClicked, the click selected the clip): Premiere's Show Clip
// Keyframes. It lists the clip's properties that can show their keyframes on the clip - Motion and Opacity of a video clip,
// Volume and Panner of an audio clip, then the parameters of each effect (TimelineView.clipKeyframeProperties); the ones that
// have keyframes carry a diamond. Choosing one draws its value line and keys over the clip (TimelineView.showClipKeyframes);
// choosing the shown one, or Hide Clip Keyframes, hides it again. The shown property is checked. Previous / Next Keyframe jump
// the playhead between the shown property's keys on the clip. View state only: nothing is edited, nothing is an undo step.
// Its own file, importing only QtQuick, so QtQuick.Controls' ContextMenu does not shadow ours.
ContextMenu {
    id: menu
    objectName: "clipKeyframesMenu"

    /// The timeline view the clip is in, and the clip's key ("c12").
    property var view: null
    property string clipKey: ""
    /// TimelineView.clipKeyframeProperties for the clip, read when the menu opens.
    property var properties: []
    readonly property bool anyShown: menu.properties.some(p => p.shown === true)
    readonly property bool anyKeyed: menu.properties.some(p => p.keyed === true)

    function openAt(target, timelineView, key, point) {
        menu.view = timelineView
        menu.clipKey = key
        menu.properties = timelineView ? timelineView.clipKeyframeProperties(key) : []
        popup(target, point.x, point.y)
    }
    function choose(propertyId) {
        if (menu.view)
            menu.view.showClipKeyframes(menu.clipKey, propertyId)
    }

    // Group label: what the menu is for.
    Item {
        implicitWidth: titleLabel.implicitWidth + Theme.space(6)
        implicitHeight: titleLabel.implicitHeight + Theme.space(2)
        Text {
            id: titleLabel
            objectName: "clipKeyframesMenuTitle"
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Show Clip Keyframes")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    ContextMenuItem {
        objectName: "clipKeyframesHide"
        text: qsTr("Hide Clip Keyframes")
        iconName: "eye-off"
        enabled: menu.anyShown
        disabledReason: qsTr("No property of this clip shows its keyframes")
        onTriggered: menu.choose("")
    }
    ContextMenuItem {
        objectName: "clipKeyframesPrevious"
        text: qsTr("Previous Keyframe")
        iconName: "chevron-left"
        enabled: menu.anyShown
        disabledReason: qsTr("Choose a property to show its keyframes first")
        onTriggered: menu.view.seekClipKeyframe(menu.clipKey, false)
    }
    ContextMenuItem {
        objectName: "clipKeyframesNext"
        text: qsTr("Next Keyframe")
        iconName: "chevron-right"
        enabled: menu.anyShown
        disabledReason: qsTr("Choose a property to show its keyframes first")
        onTriggered: menu.view.seekClipKeyframe(menu.clipKey, true)
    }
    ContextMenuSeparator {}
    Instantiator {
        model: menu.properties
        delegate: ContextMenuItem {
            id: entry
            required property var modelData
            required property int index
            objectName: "clipKeyframesProperty_" + entry.index
            text: entry.modelData.group === entry.modelData.label ? entry.modelData.label
                                                                   : qsTr("%1 · %2").arg(entry.modelData.group).arg(entry.modelData.label)
            iconName: entry.modelData.keyed ? "diamond" : ""
            hint: entry.modelData.keyed ? qsTr("Has keyframes") : qsTr("No keyframes yet: a flat line")
            checkable: true
            checked: entry.modelData.shown === true
            onTriggered: {
                entry.checked = Qt.binding(() => entry.modelData.shown === true)
                menu.choose(entry.modelData.shown === true ? "" : entry.modelData.id)
            }
        }
        // The title, the three fixed rows and the separator come first: the properties follow, in order.
        onObjectAdded: (index, object) => menu.addItem(object as ContextMenuItem)
        onObjectRemoved: (index, object) => menu.removeItem(object as ContextMenuItem)
    }
}
