pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// One page of the Settings window (0.1.3): its title and a one-line description, then its sections (SettingsSection)
// in a scrolling column. A page asks the window for things outside it (another dialog, the shortcuts editor) with
// actionRequested; the window closes first, so the dialog keeps the focus.
// UX rework: reveal(rowKey) is how the Settings search and AppSettings.openRequested land on one setting.
Item {
    id: page

    property string title: ""
    property string description: ""
    default property alias content: sections.data
    /// "media", "plugins", "audio", "extensions", "shortcuts" or "editing" (SettingsWindow.runAction).
    signal actionRequested(string what)

    /// Scrolls so that `target` is in view. `target` is a row key (a SettingsRow's or SettingsSection's `rowKey`, as
    /// AppSettings.index names it): that row also flashes and its first control takes the keyboard focus. Or any item
    /// on this page (scrolled to only). False when the page has no such row.
    function reveal(target: var): bool {
        const byKey = typeof target === "string"
        const item = byKey ? page.findRow(sections, target) : target
        if (!item)
            return false
        const y = item.mapToItem(column, 0, 0).y
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, y - Theme.space(4)))
        if (byKey) {
            item.flash()
            const control = item.nextItemInFocusChain(true)
            if (control && page.isInside(item, control))
                control.forceActiveFocus(Qt.TabFocusReason)
        }
        return true
    }
    /// The SettingsRow / SettingsSection under `from` whose rowKey is `key`, else null.
    function findRow(from, key) {
        for (let i = 0; i < from.children.length; ++i) {
            const child = from.children[i]
            if (child.rowKey === key && typeof child.flash === "function")
                return child
            const found = page.findRow(child, key)
            if (found)
                return found
        }
        return null
    }
    /// Whether `item` is `ancestor` or inside it.
    function isInside(ancestor, item) {
        for (let p = item; p; p = p.parent) {
            if (p === ancestor)
                return true
        }
        return false
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
