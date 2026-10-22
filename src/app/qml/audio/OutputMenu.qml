pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// p02 audio: a mixer strip's output assignment (Premiere's track output menu at the bottom of a strip): the Mix or a
// submix (AudioMixer.outputs), or a new submix of a format ("Create Stereo Submix"...: made and assigned in one undo
// step). A submix never lists itself.
AbstractButton {
    id: root

    /// The strip: a track or submix key ("t12").
    property string target: ""
    /// "mix" or a submix key.
    property string output: "mix"
    property string outputName: ""
    property string message: ""

    objectName: "outputMenu"
    implicitHeight: Theme.controlHeightSmall - Theme.space(1)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: qsTr("Output: %1").arg(root.outputName)
    onClicked: menu.popup(root, 0, root.height)

    background: Rectangle {
        radius: Theme.radiusPill
        color: root.hovered ? Theme.hover : Theme.sunken
        border.width: Theme.hairline
        border.color: root.output === "mix" ? Theme.border : Theme.accentStroke
    }
    contentItem: RowLayout {
        spacing: Theme.space(1) / 2
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space(1)
            horizontalAlignment: Text.AlignHCenter
            text: root.outputName
            elide: Text.ElideRight
            color: root.output === "mix" ? Theme.textSecondary : Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Icon {
            Layout.rightMargin: Theme.space(1)
            name: "chevron-down"
            size: Theme.iconXs
            color: Theme.textTertiary
        }
    }
    FocusRing {
        targetRadius: Theme.radiusPill
        visible: root.visualFocus
    }
    Loader {
        active: root.hovered
        sourceComponent: Tooltip {
            parent: root
            visible: true
            text: qsTr("Track output: where this strip goes")
        }
    }

    OptionsMenu {
        id: menu
        objectName: "outputMenuOptions"
        options: AudioMixer.outputs.filter(o => o.id !== root.target).concat(AudioMixer.submixFormats)
        current: root.output
        onChosen: id => root.message = AudioMixer.setOutput(root.target, id)
    }
}
