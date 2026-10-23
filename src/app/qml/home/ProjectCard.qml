pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A project on Home as a card (the Home page's Recent, the Projects page's grid; design language v3): its picture
// (ProjectThumb, the format in a chip), its name and when it was opened, the star and a "..." button for its menu. A
// missing project is dimmed with the Missing badge; clicking it opens the menu (Locate... / Remove from List).
// Keyboard (the list's current card is its Tab stop): Enter or Space opens, the Menu key or Shift+F10 opens the menu,
// Delete removes it from the list; the list moves between cards with the arrows (`stepRequested`).
AbstractButton {
    id: card

    /// A Home.recent entry: { url, path, name, folder, opened, openedText, format, thumbnail, missing }.
    property var project: ({})
    property string hueName: "violet"
    property int seed: 1
    /// The list's current card: the list's Tab stop; its star and "..." button take Tab after it.
    property bool current: false
    readonly property bool missing: card.project.missing === true

    signal openRequested()
    signal menuRequested(real x, real y)
    signal removeRequested()
    /// Arrow keys: -1 / +1 (left, right) or -2 / +2 (up, down: a row).
    signal stepRequested(int direction)

    function activate() {
        if (card.missing)
            card.menuRequested(card.width / 2, card.height / 2)
        else
            card.openRequested()
    }

    padding: Theme.space(2)
    bottomPadding: Theme.space(3)
    // (Kept while it has the focus: the policy may not drop Tab from the focused item.)
    focusPolicy: card.current || card.activeFocus ? Qt.StrongFocus : Qt.ClickFocus
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: card.missing ? qsTr("%1 (missing)").arg(card.project.name) : (card.project.name ?? "")
    Accessible.description: [card.project.openedText, card.project.format, card.project.path].filter(s => !!s).join(" · ")
    onClicked: card.activate()
    Keys.onReturnPressed: card.activate()
    Keys.onEnterPressed: card.activate()
    Keys.onDeletePressed: card.removeRequested()
    Keys.onMenuPressed: card.menuRequested(card.width / 2, card.height / 2)
    Keys.onLeftPressed: card.stepRequested(-1)
    Keys.onRightPressed: card.stepRequested(1)
    Keys.onUpPressed: card.stepRequested(-2)
    Keys.onDownPressed: card.stepRequested(2)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
            card.menuRequested(card.width / 2, card.height / 2)
            event.accepted = true
        }
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: card.hovered || card.visualFocus ? Theme.section : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }
    contentItem: Item {
        opacity: card.missing ? Theme.disabledOpacity + 0.2 : 1

        ProjectThumb {
            id: thumb
            width: parent.width
            height: Math.round(thumb.width * 10 / 16)
            source: card.missing ? "" : (card.project.thumbnail ?? "")
            hueName: card.hueName
            seed: card.seed
            chip: card.missing ? "" : (card.project.format ?? "")
        }
        MissingBadge {
            visible: card.missing
            x: Theme.space(2)
            y: Theme.space(2)
        }
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: thumb.bottom
            anchors.topMargin: Theme.space(2.5)
            anchors.leftMargin: Theme.space(1.5)
            anchors.rightMargin: Theme.controlHeight + Theme.space(1)
            spacing: Theme.space(0.5)

            Text {
                Layout.fillWidth: true
                text: card.project.name ?? ""
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightTitle
            }
            Text {
                Layout.fillWidth: true
                text: card.missing ? qsTr("File not found") : (card.project.openedText || card.project.folder || "")
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    StarButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Theme.space(2)
        anchors.bottomMargin: Theme.space(2.5)
        id: star
        focusPolicy: card.current || star.activeFocus ? Qt.StrongFocus : Qt.NoFocus
        path: card.project.path ?? ""
        projectName: card.project.name ?? ""
    }

    // "...": the project's menu (also right-click, the Menu key, Shift+F10).
    IconButton {
        id: more
        objectName: "homeProjectMore"
        x: card.leftPadding + card.availableWidth - width - Theme.space(1.5)
        y: card.topPadding + Theme.space(1.5)
        // Also while the star has the focus: Tab goes card, star, "..." (an item that is not shown takes no Tab).
        visible: card.hovered || card.visualFocus || star.activeFocus || more.activeFocus || card.missing
        focusPolicy: card.current || more.activeFocus ? Qt.StrongFocus : Qt.NoFocus
        compact: true
        variant: "secondary"
        iconName: "ellipsis"
        iconSize: Theme.iconXs
        tooltip: qsTr("More for %1").arg(card.project.name ?? "")
        onClicked: card.menuRequested(more.x, more.y + more.height)
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => card.menuRequested(eventPoint.position.x, eventPoint.position.y)
    }
    FocusRing {
        targetRadius: Theme.radiusPanel
        visible: card.visualFocus
    }
    HoverHandler {
        enabled: card.enabled
        cursorShape: Qt.PointingHandCursor
    }
}
