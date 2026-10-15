import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Modal dialog in the design system's look: a raised card centred on the window over a scrim, with a title,
// a body (children, stacked) and right-aligned buttons (`footerData`). Escape closes it.
Popup {
    id: control

    property string title: ""
    default property alias bodyData: body.data
    property alias footerData: footer.data

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Theme.pad + Theme.space(2)

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(4)
        Accessible.role: Accessible.Dialog
        Accessible.name: control.title

        Text {
            Layout.fillWidth: true
            text: control.title
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textLg
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: Theme.space(3)
        }

        RowLayout {
            id: footer
            Layout.alignment: Qt.AlignRight
            spacing: Theme.space(2)
        }
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.popScale; to: 1; duration: Theme.durState; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durPress; easing.type: Easing.OutCubic }
    }
}
