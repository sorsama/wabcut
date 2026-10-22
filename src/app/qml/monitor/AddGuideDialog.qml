pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// P.2 (E6): Premiere's Add Guide / Edit Guide dialog (Program Monitor > View; bridge Monitors): the orientation, the
// position in pixels from the frame's top / left edge or in percent of it, and the colour.
WabcutDialog {
    id: dialog
    objectName: "addGuideDialog"

    /// The guide being edited (-1: a new one).
    property int editing: -1
    property bool vertical: false
    property bool percent: false
    property string color: ""
    property string errorText: ""
    readonly property var colors: Monitors.guideColors

    function openNew() {
        dialog.editing = -1
        dialog.vertical = false
        dialog.percent = Monitors.rulerPercent
        dialog.color = dialog.colors[0]
        position.value = dialog.percent ? 50 : Math.round(Monitors.frameHeight / 2)
        dialog.errorText = ""
        open()
    }
    function openFor(index) {
        const g = Monitors.guides[index]
        if (g === undefined)
            return
        dialog.editing = index
        dialog.vertical = g.orientation === "vertical"
        dialog.percent = g.units === "percent"
        dialog.color = g.color
        position.value = g.value
        dialog.errorText = ""
        open()
    }
    function apply() {
        const orientation = dialog.vertical ? "vertical" : "horizontal"
        const units = dialog.percent ? "percent" : "pixels"
        const ok = dialog.editing >= 0 ? Monitors.setGuide(dialog.editing, orientation, position.value, units, dialog.color)
                                       : Monitors.addGuide(orientation, position.value, units, dialog.color) >= 0
        if (ok)
            dialog.close()
        else
            dialog.errorText = Monitors.message !== "" ? Monitors.message : qsTr("The guide cannot be added")
    }

    title: dialog.editing >= 0 ? qsTr("Edit Guide") : qsTr("Add Guide")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(120), Theme.space(120))

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(3)

        Text {
            text: qsTr("Orientation")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        PillTabs {
            objectName: "guideOrientation"
            compact: true
            model: [qsTr("Horizontal"), qsTr("Vertical")]
            currentIndex: dialog.vertical ? 1 : 0
            onActivated: index => dialog.vertical = index === 1
        }
        Text {
            text: qsTr("Position")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        RowLayout {
            spacing: Theme.space(2)
            NumberField {
                id: position
                objectName: "guidePosition"
                Layout.preferredWidth: Theme.space(32)
                from: dialog.percent ? -1000 : -100000
                to: dialog.percent ? 1000 : 100000
                decimals: dialog.percent ? 2 : 0
                stepSize: 1
                onValueModified: v => position.value = v
                Accessible.name: qsTr("Guide position")
            }
            PillTabs {
                objectName: "guideUnits"
                compact: true
                model: [qsTr("Pixels"), qsTr("Percent")]
                currentIndex: dialog.percent ? 1 : 0
                onActivated: index => {
                    // Keep the guide where it is in the other unit.
                    const size = dialog.vertical ? Monitors.frameWidth : Monitors.frameHeight
                    const percent = index === 1
                    if (percent !== dialog.percent)
                        position.value = percent ? Math.round(position.value / size * 10000) / 100 : Math.round(position.value / 100 * size)
                    dialog.percent = percent
                }
            }
        }
        Text {
            text: qsTr("Color")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Row {
            spacing: Theme.space(1)
            Repeater {
                model: dialog.colors
                delegate: Swatch {
                    required property string modelData
                    swatchColor: modelData
                    tooltip: modelData
                    checked: dialog.color.toLowerCase() === modelData
                    onClicked: dialog.color = modelData
                }
            }
        }
    }
    Text {
        objectName: "guideError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "guideOk"
            variant: "primary"
            text: qsTr("OK")
            onClicked: dialog.apply()
        }
    ]
}
