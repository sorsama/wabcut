pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One animatable number of a graphic layer, like an Effect Controls row: the stopwatch (keyframes), the value as a
// slider with its number, and the key at the playhead. `param` is { value, animated, keyed } (Graphics.layer); a drag
// is one undo step (Graphics.setValue until Graphics.endEdit), on an animated property it moves the key at the playhead.
RowLayout {
    id: row

    required property int layerId
    required property string path
    property var param: ({ value: 0, animated: false, keyed: false })
    property string label: ""
    property string unit: ""
    property real from: 0
    property real to: 100
    property real origin: from
    property real defaultValue: 0
    property real stepSize: 1
    property int decimals: 0

    objectName: "graphicParam_" + path
    Layout.fillWidth: true
    spacing: Theme.space(1)

    IconButton {
        objectName: "graphicStopwatch_" + row.path
        compact: true
        checkable: true
        checked: row.param.animated === true
        iconName: "timer"
        iconSize: Theme.iconSm
        tooltip: row.param.animated ? qsTr("Stop animating %1 (keeps its value here)").arg(row.label) : qsTr("Animate %1").arg(row.label)
        onClicked: {
            checked = Qt.binding(() => row.param.animated === true)
            Graphics.setAnimated(row.layerId, row.path, !(row.param.animated === true))
        }
    }
    PropSlider {
        objectName: "graphicSlider_" + row.path
        Layout.fillWidth: true
        label: row.label
        unit: row.unit
        from: row.from
        to: row.to
        origin: row.origin
        defaultValue: row.defaultValue
        stepSize: row.stepSize
        decimals: row.decimals
        value: Number(row.param.value ?? 0)
        Accessible.name: row.label
        onValueModified: v => Graphics.setValue(row.layerId, row.path, v)
        onGestureEnded: Graphics.endEdit()
    }
    IconButton {
        objectName: "graphicKey_" + row.path
        visible: row.param.animated === true
        compact: true
        checkable: true
        checked: row.param.keyed === true
        iconName: "diamond"
        iconSize: Theme.iconXs
        tooltip: row.param.keyed ? qsTr("Remove the keyframe of %1 at the playhead").arg(row.label)
                                 : qsTr("Add a keyframe of %1 at the playhead").arg(row.label)
        onClicked: {
            checked = Qt.binding(() => row.param.keyed === true)
            Graphics.toggleKey(row.layerId, row.path)
        }
    }
}
