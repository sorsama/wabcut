pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// One page of the Settings window (0.1.3): its title and a one-line description, then its sections (SettingsSection)
// in a scrolling column. A page asks the window for things outside it (another dialog, the shortcuts editor) with
// actionRequested; the window closes first, so the dialog keeps the focus.
Item {
    id: page

    property string title: ""
    property string description: ""
    default property alias content: sections.data
    /// "media", "plugins", "audio", "extensions" or "shortcuts" (SettingsWindow.runAction).
    signal actionRequested(string what)

    /// Scrolls so that `item` (anything on this page) is in view.
    function reveal(item: Item) {
        if (!item)
            return
        const y = item.mapToItem(column, 0, 0).y
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, y - Theme.space(4)))
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + Theme.space(8)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: column
            x: Theme.space(6)
            y: Theme.space(5)
            width: flick.width - Theme.space(12)
            spacing: Theme.space(6)

            Column {
                width: parent.width
                spacing: Theme.space(1)

                Text {
                    width: parent.width
                    text: page.title
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.textTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Text {
                    width: parent.width
                    visible: page.description !== ""
                    text: page.description
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }

            Column {
                id: sections
                width: parent.width
                spacing: Theme.space(6)
            }
        }
    }
}
