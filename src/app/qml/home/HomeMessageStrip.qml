pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A message strip under Home's header (design language v3): a tinted card with an icon, an optional bold title, the
// message (wraps; a long one - an import's notes - is four lines high and scrolls, `<objectName>Text`), actions on the
// right and a close x. The editor's toast is not visible over Home, so Home's errors, import guidance and notes show
// here.
Rectangle {
    id: strip

    /// "info", "warning" or "danger".
    property string tone: "info"
    property string title: ""
    property string message: ""
    property bool closable: true
    /// Buttons or links at the right (PillButton, HomeLink).
    default property alias actions: actionRow.data
    readonly property color toneColor: strip.tone === "danger" ? Theme.danger : strip.tone === "warning" ? Theme.warning : Theme.info
    /// One line of the message (it shows four before it scrolls).
    readonly property real lineHeight: Math.ceil(messageMetrics.height)

    signal closeRequested()

    FontMetrics {
        id: messageMetrics
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    implicitHeight: content.implicitHeight + Theme.space(5)
    radius: Theme.radiusCard
    color: Qt.alpha(strip.toneColor, Theme.dark ? 0.12 : 0.10)
    border.width: Theme.hairline
    border.color: Qt.alpha(strip.toneColor, 0.55)
    Accessible.role: Accessible.AlertMessage
    Accessible.name: strip.title !== "" ? strip.title : strip.message
    Accessible.description: strip.title !== "" ? strip.message : ""

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.space(4)
        anchors.rightMargin: Theme.space(2)
        spacing: Theme.space(3)

        Icon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: Theme.space(0.5)
            name: strip.tone === "info" ? "book-open" : "triangle-alert"
            size: Theme.iconSm
            color: strip.toneColor
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space(0.5)
            Text {
                Layout.fillWidth: true
                visible: strip.title !== ""
                text: strip.title
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightLabel
            }
            // The message in full: at most four lines high, the rest scrolls (the wheel, the scroll bar, or the arrow
            // and page keys once it has the focus - Tab reaches it when there is more to read).
            Item {
                Layout.fillWidth: true
                implicitHeight: Math.min(messageText.implicitHeight, strip.lineHeight * 4)
                visible: strip.message !== ""

                Flickable {
                    id: messageFlick
                    objectName: strip.objectName + "Text"
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: messageText.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height + 0.5
                    activeFocusOnTab: interactive
                    Accessible.role: Accessible.StaticText
                    Accessible.name: strip.message
                    ScrollBar.vertical: ThinScrollBar {}
                    Keys.onUpPressed: messageFlick.scrollBy(-strip.lineHeight)
                    Keys.onDownPressed: messageFlick.scrollBy(strip.lineHeight)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                            messageFlick.scrollBy((event.key === Qt.Key_PageUp ? -1 : 1) * messageFlick.height)
                            event.accepted = true
                        }
                    }

                    function scrollBy(dy: real) {
                        messageFlick.contentY = Math.max(0, Math.min(messageFlick.contentHeight - messageFlick.height,
                                                                     messageFlick.contentY + dy))
                    }

                    Text {
                        id: messageText
                        // (Room for the scroll bar always: a width that followed it would change the height it decides.)
                        width: messageFlick.width - Theme.space(3)
                        text: strip.message
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
                FocusRing {
                    targetRadius: Theme.radiusBadge
                    visible: messageFlick.activeFocus && messageFlick.interactive
                }
            }
        }
        RowLayout {
            id: actionRow
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.space(2)
        }
        IconButton {
            objectName: strip.objectName + "Close"
            Layout.alignment: Qt.AlignVCenter
            visible: strip.closable
            compact: true
            iconName: "x"
            iconSize: Theme.iconXs
            tooltip: qsTr("Dismiss")
            onClicked: strip.closeRequested()
        }
    }
}
