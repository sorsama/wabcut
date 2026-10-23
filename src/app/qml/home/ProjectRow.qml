pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A project on Home as a row (the Projects page's list view; design language v3): a small picture, the name over its
// folder, when it was opened, the format (or the Missing badge), the star and a "..." button for its menu. The same
// keys and menu as ProjectCard; Up/Down move between rows.
AbstractButton {
    id: row

    /// A Home.recent entry: { url, path, name, folder, opened, openedText, format, thumbnail, missing }.
    property var project: ({})
    property string hueName: "violet"
    property int seed: 1
    property bool current: false
    /// Widths of the Opened and Format columns (the list's header uses the same).
    property real openedWidth: Theme.space(36)
    property real formatWidth: Theme.space(28)
    readonly property bool missing: row.project.missing === true

    signal openRequested()
    signal menuRequested(real x, real y)
    signal removeRequested()
    /// Up / Down: -1 / +1.
    signal stepRequested(int direction)

    function activate() {
        if (row.missing)
            row.menuRequested(row.width - Theme.space(20), row.height)
        else
            row.openRequested()
    }

    implicitHeight: Theme.space(14)
    leftPadding: Theme.space(3)
    rightPadding: Theme.space(2)
    // (Kept while it has the focus: the policy may not drop Tab from the focused item.)
    focusPolicy: row.current || row.activeFocus ? Qt.StrongFocus : Qt.ClickFocus
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: row.missing ? qsTr("%1 (missing)").arg(row.project.name) : (row.project.name ?? "")
    Accessible.description: [row.project.openedText, row.project.format, row.project.path].filter(s => !!s).join(" · ")
    onClicked: row.activate()
    Keys.onReturnPressed: row.activate()
    Keys.onEnterPressed: row.activate()
    Keys.onDeletePressed: row.removeRequested()
    Keys.onMenuPressed: row.menuRequested(row.width - Theme.space(20), row.height)
    Keys.onUpPressed: row.stepRequested(-1)
    Keys.onDownPressed: row.stepRequested(1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
            row.menuRequested(row.width - Theme.space(20), row.height)
            event.accepted = true
        }
    }

    background: Rectangle {
        radius: Theme.radiusCard - Theme.space(0.5)
        color: row.hovered || row.visualFocus ? Theme.section : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }
    contentItem: RowLayout {
        spacing: Theme.space(3.5)

        ProjectThumb {
            implicitWidth: Theme.space(16)
            implicitHeight: Theme.space(10)
            opacity: row.missing ? Theme.disabledOpacity : 1
            source: row.missing ? "" : (row.project.thumbnail ?? "")
            hueName: row.hueName
            seed: row.seed
            cornerRadius: Theme.space(2.5)
            detailed: false
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: row.project.name ?? ""
                elide: Text.ElideRight
                color: row.missing ? Theme.textTertiary : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightTitle
            }
            Text {
                Layout.fillWidth: true
                text: row.project.folder ?? ""
                elide: Text.ElideMiddle
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        Text {
            Layout.preferredWidth: row.openedWidth
            text: row.project.openedText ?? ""
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Item {
            Layout.preferredWidth: row.formatWidth
            implicitHeight: Theme.controlHeightSmall
            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !row.missing
                text: row.project.format ?? ""
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            MissingBadge {
                visible: row.missing
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        StarButton {
            id: star
            focusPolicy: row.current || star.activeFocus ? Qt.StrongFocus : Qt.NoFocus
            path: row.project.path ?? ""
            projectName: row.project.name ?? ""
        }
        IconButton {
            id: more
            objectName: "homeProjectMore"
            focusPolicy: row.current || more.activeFocus ? Qt.StrongFocus : Qt.NoFocus
            compact: true
            iconName: "ellipsis"
            iconSize: Theme.iconXs
            tooltip: qsTr("More for %1").arg(row.project.name ?? "")
            onClicked: {
                const p = more.mapToItem(row, 0, more.height)
                row.menuRequested(p.x, p.y)
            }
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => row.menuRequested(eventPoint.position.x, eventPoint.position.y)
    }
    FocusRing {
        targetRadius: Theme.radiusCard - Theme.space(0.5)
        visible: row.visualFocus
    }
    HoverHandler {
        enabled: row.enabled
        cursorShape: Qt.PointingHandCursor
    }
}
