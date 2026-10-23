pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Home's Learn page (design language v3): compact rows (an icon, a title, one short detail, where it leads), grouped:
//   Start here   the sample project (`sampleRequested`), the tour, the Keyboard Trainer and What's new
//                (`helpRequested`: Main closes Home first);
//   User guide   the website's guides (Home.links, group "guide"), opened in the browser;
//   Support      release notes, roadmap, report an issue (group "support").
// Two columns when there is room; the page scrolls inside itself only if a short window needs it.
Flickable {
    id: page
    objectName: "homeLearnPage"

    property bool revealed: true

    signal sampleRequested()
    signal helpRequested(string what)

    readonly property bool twoColumns: page.width >= Theme.space(200)
    readonly property var guideLinks: Home.links.filter(l => l.group === "guide")
    readonly property var supportLinks: Home.links.filter(l => l.group !== "guide")
    readonly property var linkIcons: ({
        gettingStarted: "play", theWindow: "layout-grid", fromPremiere: "arrow-left-right",
        exchangeTimelines: "import", shortcuts: "keyboard", gamingVideo: "gamepad-2", shorts: "film",
        videoEssay: "type", documentary: "clapperboard", allGuides: "book-open", releaseNotes: "sparkles",
        roadmap: "spline", reportIssue: "message-square"
    })

    /// The first row takes the keyboard focus.
    function focusFirst() {
        sampleRow.forceActiveFocus(Qt.TabFocusReason)
    }

    contentWidth: width
    contentHeight: grid.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    interactive: contentHeight > height
    ScrollBar.vertical: ThinScrollBar {}

    // A row: an icon in a tinted circle, the title over one line of detail, and where it leads (the website or the
    // app).
    component LearnRow: AbstractButton {
        id: row
        property string iconName: "book-open"
        property string hueName: "violet"
        property string detail: ""
        property bool external: false
        Layout.fillWidth: true
        implicitHeight: Theme.space(12.5)
        leftPadding: Theme.space(2)
        rightPadding: Theme.space(3)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: row.external ? Accessible.Link : Accessible.Button
        Accessible.name: text
        Accessible.description: row.external ? qsTr("%1 (opens the website)").arg(row.detail) : row.detail
        Keys.onReturnPressed: row.clicked()
        Keys.onEnterPressed: row.clicked()

        background: Rectangle {
            radius: Theme.radiusCard - Theme.space(0.5)
            color: row.hovered || row.visualFocus ? Theme.section : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        contentItem: RowLayout {
            spacing: Theme.space(3)
            Rectangle {
                implicitWidth: Theme.controlHeight
                implicitHeight: Theme.controlHeight
                radius: height / 2
                color: Qt.alpha(Theme.hue(row.hueName), 0.16)
                Icon {
                    anchors.centerIn: parent
                    name: row.iconName
                    size: Theme.iconSm
                    color: Theme.dark ? Theme.hue(row.hueName) : Theme.textPrimary
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: row.text
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
                Text {
                    Layout.fillWidth: true
                    visible: row.detail !== ""
                    text: row.detail
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            Icon {
                name: row.external ? "external-link" : "chevron-right"
                size: Theme.iconSm
                color: row.hovered ? Theme.accentStroke : Theme.textTertiary
            }
        }
        FocusRing {
            targetRadius: Theme.radiusCard - Theme.space(0.5)
            visible: row.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    GridLayout {
        id: grid
        width: page.width - (page.interactive ? Theme.space(3) : 0)
        columns: page.twoColumns ? 2 : 1
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.gap

            HomeCard {
                objectName: "homeLearnStart"
                Layout.fillWidth: true
                order: 0
                revealed: page.revealed
                title: qsTr("Start here")
                hue: "lime"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(0.5)
                    LearnRow {
                        id: sampleRow
                        objectName: "homeSampleProject"
                        iconName: "clapperboard"
                        hueName: "lime"
                        text: qsTr("Explore the sample project")
                        detail: qsTr("Three sequences, ready to play with")
                        onClicked: page.sampleRequested()
                    }
                    LearnRow {
                        objectName: "homeLearnTour"
                        iconName: "mouse-pointer-2"
                        hueName: "violet"
                        text: qsTr("Take the tour")
                        detail: qsTr("Library, Timeline and Export in a minute")
                        onClicked: page.helpRequested("tour")
                    }
                    LearnRow {
                        objectName: "homeLearnTrainer"
                        iconName: "keyboard"
                        hueName: "blue"
                        text: qsTr("Keyboard Trainer")
                        detail: qsTr("Learn the Premiere keys by playing")
                        onClicked: page.helpRequested("trainer")
                    }
                    LearnRow {
                        objectName: "homeLearnWhatsNew"
                        iconName: "sparkles"
                        hueName: "pink"
                        text: qsTr("What's new")
                        detail: qsTr("The highlights of Wabcut %1").arg(Qt.application.version)
                        onClicked: page.helpRequested("whatsNew")
                    }
                }
            }

            HomeCard {
                objectName: "homeLearnSupport"
                Layout.fillWidth: true
                visible: page.supportLinks.length > 0
                order: 2
                revealed: page.revealed
                title: qsTr("Support")
                hue: "mint"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(0.5)
                    Repeater {
                        model: page.supportLinks
                        delegate: LearnRow {
                            required property var modelData
                            objectName: "homeLink_" + modelData.id
                            iconName: page.linkIcons[modelData.id] ?? "external-link"
                            hueName: "mint"
                            external: true
                            text: modelData.title
                            detail: modelData.detail
                            onClicked: Qt.openUrlExternally(modelData.url)
                        }
                    }
                }
            }
        }

        HomeCard {
            objectName: "homeLearnGuide"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            order: 1
            revealed: page.revealed
            title: qsTr("User guide")
            hue: "orange"
            trailing: [
                Text {
                    text: qsTr("wabcut.zyphite.com")
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            ]

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(0.5)
                Repeater {
                    model: page.guideLinks
                    delegate: LearnRow {
                        required property var modelData
                        objectName: "homeLink_" + modelData.id
                        iconName: page.linkIcons[modelData.id] ?? "book-open"
                        hueName: "orange"
                        external: true
                        text: modelData.title
                        detail: modelData.detail
                        onClicked: Qt.openUrlExternally(modelData.url)
                    }
                }
            }
        }
    }
}
