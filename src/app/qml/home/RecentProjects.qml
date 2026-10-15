pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes

// Projects (Home.recent, newest first), design language v3: a Recent / Starred filter, a count and a grid / list
// switch over the cards. A card shows the project's picture (ProjectThumb), its name and when it was last opened, with
// a star (Preferences.setStarred) at its corner. The search in Home's header narrows the list by name (`query`). A
// project whose file is gone is dimmed and flagged "Missing"; clicking it offers to remove it from the list.
HomeCard {
    id: card
    objectName: "homeRecentCard"

    /// Home's search text: only projects whose name contains it (any case).
    property string query: ""
    /// 0: every recent project, 1: the starred ones.
    property int filter: 0
    property bool listView: false
    // Read again whenever the starred list changes.
    readonly property var starredPaths: Preferences.homeStarred !== "" ? Preferences.starredProjects() : []
    readonly property var matching: {
        const q = card.query.trim().toLowerCase()
        return Home.recent.filter(p => q === "" || String(p.name).toLowerCase().includes(q))
    }
    readonly property var shown: card.filter === 1 ? card.matching.filter(p => card.starredPaths.includes(p.path))
                                                    : card.matching
    readonly property int starredCount: Home.recent.filter(p => card.starredPaths.includes(p.path)).length

    signal openRequested(url file)      // an empty url: choose a file

    // The star in a project's corner: lime and filled when starred.
    component StarButton: AbstractButton {
        id: star

        required property string path
        required property string projectName
        // Read again whenever the starred list changes (Preferences.homeStarred).
        readonly property bool starred: Preferences.homeStarred !== "" && Preferences.isStarred(path)

        objectName: "homeRecentStar"
        implicitWidth: Theme.controlHeight
        implicitHeight: Theme.controlHeight
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.name: starred ? qsTr("Unstar %1").arg(projectName) : qsTr("Star %1").arg(projectName)
        Accessible.checkable: true
        Accessible.checked: starred
        onClicked: Preferences.setStarred(star.path, !star.starred)

        background: Rectangle {
            radius: height / 2
            color: star.starred ? Theme.brandLime : star.hovered ? Theme.hover : Theme.raised
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        contentItem: Item {
            Shape {
                id: glyph
                anchors.centerIn: parent
                width: Theme.iconXs
                height: Theme.iconXs
                preferredRendererType: Shape.CurveRenderer
                readonly property real u: width / 24
                ShapePath {
                    strokeColor: star.starred ? Theme.clipInk : Theme.textSecondary
                    strokeWidth: 2 * glyph.u
                    joinStyle: ShapePath.RoundJoin
                    fillColor: star.starred ? Theme.clipInk : "transparent"
                    PathPolyline {
                        path: [Qt.point(12, 2), Qt.point(15.1, 8.3), Qt.point(22, 9.3), Qt.point(17, 14.1),
                               Qt.point(18.2, 21), Qt.point(12, 17.8), Qt.point(5.8, 21), Qt.point(7, 14.1),
                               Qt.point(2, 9.3), Qt.point(8.9, 8.3), Qt.point(12, 2)]
                               .map(p => Qt.point(p.x * glyph.u, p.y * glyph.u))
                    }
                }
            }
        }
        FocusRing {
            targetRadius: star.height / 2
            visible: star.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    // A missing project's badge.
    component MissingBadge: Rectangle {
        implicitWidth: missingRow.implicitWidth + Theme.space(3)
        implicitHeight: Theme.space(5.5)
        radius: height / 2
        color: Theme.clipInk

        Row {
            id: missingRow
            anchors.centerIn: parent
            spacing: Theme.space(1)
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "triangle-alert"
                size: Theme.iconXs
                color: Theme.warning
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Missing")
                color: Theme.contentWhite
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Theme.weightLabel
            }
        }
    }

    // Filter, count, grid / list.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        PillTabs {
            objectName: "homeProjectFilter"
            Layout.preferredWidth: Theme.space(56)
            model: [qsTr("Recent %1").arg(Home.recent.length), qsTr("Starred %1").arg(card.starredCount)]
            currentIndex: card.filter
            Accessible.name: qsTr("Show projects")
            onActivated: index => card.filter = index
        }
        Item { Layout.fillWidth: true }
        Text {
            objectName: "homeProjectCount"
            text: qsTr("%n project(s)", "", card.shown.length)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Rectangle {
            implicitWidth: viewRow.implicitWidth + Theme.space(2)
            implicitHeight: viewRow.implicitHeight + Theme.space(2)
            radius: height / 2
            color: Theme.sunken
            Row {
                id: viewRow
                anchors.centerIn: parent
                spacing: Theme.space(0.5)
                IconButton {
                    objectName: "homeGridView"
                    compact: true
                    iconName: "layout-grid"
                    iconSize: Theme.iconXs
                    checked: !card.listView
                    tooltip: qsTr("Show projects as a grid")
                    onClicked: card.listView = false
                }
                IconButton {
                    objectName: "homeListView"
                    compact: true
                    iconName: "list"
                    iconSize: Theme.iconXs
                    checked: card.listView
                    tooltip: qsTr("Show projects as a list")
                    onClicked: card.listView = true
                }
            }
        }
    }

    Text {
        objectName: "homeProjectsEmpty"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space(2)
        Layout.bottomMargin: Theme.space(2)
        visible: card.shown.length === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: Home.recent.length === 0 ? qsTr("Projects you open or save appear here.")
            : card.filter === 1 && card.query.trim() === "" ? qsTr("Star a project to keep it here.")
            : qsTr("No projects match. Try another name or filter.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // ---- Grid ----
    Flow {
        id: flow
        objectName: "homeRecentList"
        Layout.fillWidth: true
        visible: !card.listView && card.shown.length > 0
        spacing: Theme.space(3)

        readonly property int columns: Math.max(1, Math.floor((width + spacing) / (Theme.space(56) + spacing)))
        readonly property real cardWidth: Math.floor((width - spacing * (columns - 1)) / columns)

        Repeater {
            model: card.listView ? [] : card.shown
            delegate: AbstractButton {
                id: recent
                required property var modelData
                required property int index
                objectName: "homeRecent_" + index
                width: flow.cardWidth
                padding: Theme.space(2)
                bottomPadding: Theme.space(3)
                implicitHeight: thumb.height + info.implicitHeight + Theme.space(2.5) + topPadding + bottomPadding
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.name: modelData.missing ? qsTr("%1 (missing)").arg(modelData.name) : modelData.name
                Accessible.description: modelData.path
                onClicked: {
                    if (modelData.missing)
                        removePrompt.visible = !removePrompt.visible
                    else
                        card.openRequested(modelData.url)
                }

                background: Rectangle {
                    radius: Theme.radiusPanel
                    color: recent.hovered || recent.visualFocus ? Theme.section : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
                contentItem: Item {
                    opacity: recent.modelData.missing ? Theme.disabledOpacity + 0.2 : 1

                    ProjectThumb {
                        id: thumb
                        width: parent.width
                        height: Math.round(thumb.width * 10 / 16)
                        source: recent.modelData.missing ? "" : recent.modelData.thumbnail
                        hueName: Theme.hueOrder[recent.index % Theme.hueOrder.length]
                        seed: recent.index + 1
                        chip: recent.modelData.missing ? "" : recent.modelData.format
                    }
                    MissingBadge {
                        visible: recent.modelData.missing
                        x: Theme.space(2)
                        y: Theme.space(2)
                    }
                    ColumnLayout {
                        id: info
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: thumb.bottom
                        anchors.topMargin: Theme.space(2.5)
                        anchors.leftMargin: Theme.space(1.5)
                        anchors.rightMargin: Theme.controlHeight + Theme.space(1)
                        spacing: Theme.space(0.5)

                        Text {
                            Layout.fillWidth: true
                            text: recent.modelData.name
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightTitle
                        }
                        Text {
                            Layout.fillWidth: true
                            text: recent.modelData.openedText
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
                    path: recent.modelData.path
                    projectName: recent.modelData.name
                }

                // A missing project: remove it from the list (or keep it, if its drive is just not connected).
                Rectangle {
                    id: removePrompt
                    objectName: "homeRecentRemovePrompt"
                    visible: false
                    x: recent.leftPadding
                    y: recent.topPadding
                    width: thumb.width
                    height: thumb.height
                    radius: Theme.radiusCard - Theme.space(0.5)
                    color: Theme.scrim

                    Column {
                        anchors.centerIn: parent
                        spacing: Theme.space(2)
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: removePrompt.width - Theme.space(6)
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: qsTr("The file is not there any more.")
                            color: Theme.scrimText
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        PillButton {
                            anchors.horizontalCenter: parent.horizontalCenter
                            compact: true
                            variant: "primary"
                            text: qsTr("Remove from list")
                            onClicked: Home.removeRecent(recent.modelData.url)
                        }
                    }
                }

                // Remove (hover)
                IconButton {
                    objectName: "homeRecentRemove"
                    x: recent.leftPadding + thumb.width - width - Theme.space(1.5)
                    y: recent.topPadding + Theme.space(1.5)
                    visible: recent.hovered && !recent.modelData.missing
                    compact: true
                    variant: "secondary"
                    iconName: "x"
                    iconSize: Theme.iconXs
                    tooltip: qsTr("Remove from recent projects")
                    onClicked: Home.removeRecent(recent.modelData.url)
                }

                FocusRing {
                    targetRadius: Theme.radiusPanel
                    visible: recent.visualFocus
                }

                HoverHandler {
                    enabled: recent.enabled
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
    }

    // ---- List ----
    ColumnLayout {
        objectName: "homeRecentRows"
        Layout.fillWidth: true
        visible: card.listView && card.shown.length > 0
        spacing: Theme.space(0.5)

        // Column labels.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space(3)
            Layout.rightMargin: Theme.space(3)
            spacing: Theme.space(3.5)
            Item { implicitWidth: Theme.space(16) }
            Text {
                Layout.fillWidth: true
                text: qsTr("Name")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking
            }
            Text {
                Layout.preferredWidth: Theme.space(40)
                text: qsTr("Opened")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking
            }
            Text {
                Layout.preferredWidth: Theme.space(30)
                text: qsTr("Format")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking
            }
            Item { implicitWidth: Theme.controlHeight * 2 + Theme.space(2) }
        }

        Repeater {
            model: card.listView ? card.shown : []
            delegate: AbstractButton {
                id: row
                required property var modelData
                required property int index
                objectName: "homeRecent_" + index
                Layout.fillWidth: true
                implicitHeight: Theme.space(14)
                leftPadding: Theme.space(3)
                rightPadding: Theme.space(3)
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.name: modelData.missing ? qsTr("%1 (missing)").arg(modelData.name) : modelData.name
                Accessible.description: modelData.path
                onClicked: {
                    if (!modelData.missing)
                        card.openRequested(modelData.url)
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
                        opacity: row.modelData.missing ? Theme.disabledOpacity : 1
                        source: row.modelData.missing ? "" : row.modelData.thumbnail
                        hueName: Theme.hueOrder[row.index % Theme.hueOrder.length]
                        seed: row.index + 1
                        cornerRadius: Theme.space(2.5)
                        detailed: false
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        elide: Text.ElideRight
                        color: row.modelData.missing ? Theme.textTertiary : Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Theme.weightTitle
                    }
                    Text {
                        Layout.preferredWidth: Theme.space(40)
                        text: row.modelData.openedText
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Item {
                        Layout.preferredWidth: Theme.space(30)
                        implicitHeight: Theme.controlHeightSmall
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !row.modelData.missing
                            text: row.modelData.format
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        MissingBadge {
                            visible: row.modelData.missing
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Item {
                        implicitWidth: Theme.controlHeight
                        implicitHeight: Theme.controlHeight
                        IconButton {
                            objectName: "homeRecentRemove"
                            anchors.centerIn: parent
                            visible: row.hovered || row.modelData.missing
                            compact: true
                            iconName: row.modelData.missing ? "trash-2" : "x"
                            iconSize: Theme.iconXs
                            tooltip: row.modelData.missing ? qsTr("Remove from list") : qsTr("Remove from recent projects")
                            onClicked: Home.removeRecent(row.modelData.url)
                        }
                    }
                    StarButton {
                        path: row.modelData.path
                        projectName: row.modelData.name
                    }
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
        }
    }
}
