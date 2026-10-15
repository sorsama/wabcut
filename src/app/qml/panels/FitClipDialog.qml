pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Fit Clip (M5h): Insert/Overwrite with all four marks set, and the source In/Out and sequence In/Out differ in
// length. Premiere's five choices; "Always use this choice" keeps it in Preferences.fitClipChoice and later
// four-point edits use it without asking. Main runs the edit with the choice (SourceMonitor.edit).
WabcutDialog {
    id: dialog
    objectName: "fitClipDialog"

    property bool insert: true
    property string choice: "speed"
    property bool remember: false

    signal chosen(string fit, bool remember)

    function ask(isInsert) {
        insert = isInsert
        choice = "speed"
        remember = false
        open()
    }
    function accept() {
        close()
        chosen(choice, remember)
    }

    readonly property var choices: [
        { id: "speed", label: qsTr("Change clip speed (Fit to Fill)"),
          hint: qsTr("The marked source plays faster or slower to fill the sequence In to Out exactly.") },
        { id: "ignoreSourceIn", label: qsTr("Ignore source In point"),
          hint: qsTr("The clip ends at the source Out; the sequence In to Out sets its length.") },
        { id: "ignoreSourceOut", label: qsTr("Ignore source Out point"),
          hint: qsTr("The clip starts at the source In; the sequence In to Out sets its length.") },
        { id: "ignoreSequenceIn", label: qsTr("Ignore sequence In point"),
          hint: qsTr("The whole marked source, ending at the sequence Out.") },
        { id: "ignoreSequenceOut", label: qsTr("Ignore sequence Out point"),
          hint: qsTr("The whole marked source, starting at the sequence In.") }
    ]

    title: qsTr("Fit Clip")

    Text {
        Layout.fillWidth: true
        //: Fit Clip dialog; %1 = source In/Out duration, %2 = sequence In/Out duration
        text: qsTr("The source In to Out (%1) and the sequence In to Out (%2) have different lengths. How should the clip fit?")
                  .arg(SourceMonitor.inOutText).arg(SourceMonitor.sequenceInOutText)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)

        Repeater {
            model: dialog.choices
            delegate: AbstractButton {
                id: option
                required property var modelData
                readonly property bool selected: dialog.choice === modelData.id
                objectName: "fitChoice_" + modelData.id
                Layout.fillWidth: true
                padding: Theme.space(2)
                leftPadding: Theme.space(3)
                rightPadding: Theme.space(3)
                implicitHeight: implicitContentHeight + topPadding + bottomPadding
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.description: modelData.hint
                Accessible.checked: selected
                onClicked: dialog.choice = modelData.id
                onDoubleClicked: {
                    dialog.choice = modelData.id
                    dialog.accept()
                }

                background: Rectangle {
                    radius: Theme.radiusField
                    color: option.selected ? Theme.accentWash : option.hovered ? Theme.hover : "transparent"
                    border.width: option.selected ? Theme.hairline : 0
                    border.color: Theme.accentStroke
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
                contentItem: RowLayout {
                    spacing: Theme.space(2)

                    // Radio mark: a ring, filled when chosen.
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Theme.space(0.25)
                        implicitWidth: Theme.iconSm
                        implicitHeight: Theme.iconSm
                        radius: width / 2
                        color: "transparent"
                        border.width: Theme.focusRingWidth
                        border.color: option.selected ? Theme.accentStroke : Theme.textTertiary

                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width / 2
                            height: width
                            radius: width / 2
                            color: Theme.accentStroke
                            visible: option.selected
                        }
                    }
                    ColumnLayout {
                        id: optionColumn
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: option.modelData.label
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: option.selected ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            Layout.fillWidth: true
                            text: option.modelData.hint
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.Wrap
                        }
                    }
                }
                FocusRing {
                    anchors.fill: parent
                    targetRadius: Theme.radiusField
                    visible: option.visualFocus
                }
                HoverHandler {
                    enabled: option.enabled
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
    }

    Toggle {
        objectName: "fitClipRemember"
        text: qsTr("Always use this choice")
        checked: dialog.remember
        onToggled: dialog.remember = checked
    }

    footerData: [
        PillButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "fitClipOk"
            text: dialog.insert ? qsTr("Insert") : qsTr("Overwrite")
            variant: "primary"
            onClicked: dialog.accept()
        }
    ]
}
