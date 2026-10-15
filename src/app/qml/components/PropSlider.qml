import QtQuick
import QtQuick.Layouts

// Property row for the clip editor: icon · label · slider · NumberField (with unit suffix) · reset (↺).
// Below Theme.propCompactWidth the icon is dropped so the slider keeps a usable length.
// The slider fills from `origin` (e.g. 0 for bipolar properties such as rotation or pan) to the value.
// Bind `value` to the model and write `valueModified` back to it. The row also keeps its own copy of the
// value, so it stays correct when the owner does not write back, and a binding on `value` is never broken.
Item {
    id: root

    property string label: ""
    property string iconName: ""
    property string unit: ""
    property real from: 0
    property real to: 100
    property real value: 0
    property real defaultValue: 0
    property real origin: from
    property real stepSize: 1
    property int decimals: 0
    // The value currently shown: follows `value`, and user edits update it immediately.
    property real shownValue: 0
    readonly property bool modified: Math.abs(shownValue - defaultValue) > Math.pow(10, -decimals) / 2
    readonly property bool compactLayout: width > 0 && width < Theme.propCompactWidth

    signal valueModified(real value)
    /// The user finished a drag, scrub, typed edit or reset (owners close their undo merge group here).
    signal gestureEnded()

    function setFromUser(v) {
        const q = Math.pow(10, decimals)
        const next = Math.min(to, Math.max(from, Math.round(v * q) / q))
        if (next === shownValue)
            return
        shownValue = next
        valueModified(next)
    }

    onValueChanged: shownValue = value
    Component.onCompleted: shownValue = value

    implicitWidth: row.implicitWidth
    implicitHeight: Theme.controlHeight

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: Theme.space(1.5)

        Icon {
            Layout.alignment: Qt.AlignVCenter
            name: root.iconName
            size: Theme.iconSm
            color: root.modified ? Theme.accentStroke : Theme.textTertiary
            visible: root.iconName !== "" && !root.compactLayout
        }

        Text {
            Layout.preferredWidth: root.compactLayout ? Theme.propLabelWidth - Theme.space(3) : Theme.propLabelWidth
            Layout.alignment: Qt.AlignVCenter
            text: root.label
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }

        ValueSlider {
            id: slider

            Layout.fillWidth: true
            Layout.minimumWidth: Theme.space(10)
            Layout.alignment: Qt.AlignVCenter
            from: root.from
            to: root.to
            origin: root.origin
            value: root.shownValue
            stepSize: root.stepSize
            onMoved: root.setFromUser(value)
            onPressedChanged: {
                if (!pressed)
                    root.gestureEnded()
            }

            Accessible.name: root.label
        }

        NumberField {
            Layout.preferredWidth: Theme.numberFieldWidth
            Layout.alignment: Qt.AlignVCenter
            from: root.from
            to: root.to
            value: root.shownValue
            stepSize: root.stepSize
            dragStep: (root.to - root.from) / 400
            decimals: root.decimals
            suffix: root.unit
            onValueModified: v => root.setFromUser(v)
            onEditingFinished: root.gestureEnded()
            onScrubFinished: root.gestureEnded()
            Accessible.name: root.label
        }

        IconButton {
            Layout.alignment: Qt.AlignVCenter
            compact: true
            iconName: "rotate-ccw"
            iconSize: Theme.iconSm
            tooltip: qsTr("Reset %1").arg(root.label)
            enabled: root.modified
            opacity: root.modified ? 1 : 0
            onClicked: {
                root.setFromUser(root.defaultValue)
                root.gestureEnded()
            }

            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        }
    }
}
