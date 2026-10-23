pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Continue (the Home page, design language v3): the project opened most recently that is still there (Home.recent),
// large: its 16:9 picture, its name, when it was opened, its format and folder, Open and Show in Explorer. When it is
// the project open behind Home, Open goes back to it (`returnRequested`) instead of loading it again. With no recent
// project: the echo mark and one quiet line (Start beside it has New project and Open...). The card fills the height
// its page gives it; the picture follows.
HomeCard {
    id: card
    objectName: "homeContinueCard"

    readonly property var project: {
        for (const p of Home.recent) {
            if (!p.missing)
                return p
        }
        return null
    }
    /// The project is the one open behind Home.
    readonly property bool current: card.project !== null && ProjectFile.filePath !== ""
                                    && card.samePath(card.project.path, ProjectFile.filePath)

    signal openRequested(url file)
    signal returnRequested()
    signal chooseProjectRequested()
    signal revealRequested(url file)

    function samePath(a: string, b: string): bool {
        const norm = s => String(s).replace(/\\/g, "/").toLowerCase()
        return norm(a) === norm(b)
    }
    function openProject() {
        if (card.project === null)
            card.chooseProjectRequested()
        else if (card.current)
            card.returnRequested()
        else
            card.openRequested(card.project.url)
    }

    title: qsTr("Continue")
    hue: "violet"

    // ---- The last project ----
    Item {
        id: row
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: card.project !== null
        implicitHeight: Theme.space(40)

        readonly property int gutter: Theme.space(5)
        // The picture: 16:9 at the card's height, at most about 60% of its width; picture and text centred in the
        // height the card has.
        readonly property real thumbWidth: Math.max(0, Math.min(Math.round((width - gutter) * 0.6), Math.round(height * 16 / 9)))
        readonly property real contentTop: Math.max(0, Math.round((height - Math.max(thumb.height, info.implicitHeight)) / 2))

        ProjectThumb {
            id: thumb
            objectName: "homeContinueThumb"
            y: row.contentTop
            width: row.thumbWidth
            height: Math.round(thumb.width * 9 / 16)
            cornerRadius: Theme.radiusCard
            source: card.project ? card.project.thumbnail : ""
            hueName: "violet"
            seed: 1
        }

        ColumnLayout {
            id: info
            x: thumb.width + row.gutter
            y: row.contentTop
            width: row.width - x
            height: Math.max(thumb.height, Math.min(row.height, implicitHeight))
            spacing: Theme.space(2)

            Text {
                objectName: "homeContinueName"
                Layout.fillWidth: true
                text: card.project ? card.project.name : ""
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textDisplay + 2
                font.weight: Theme.weightTitle
                font.letterSpacing: -0.5
            }
            Text {
                objectName: "homeContinueMeta"
                Layout.fillWidth: true
                text: card.project ? [card.project.openedText, card.project.format].filter(s => s !== "").join(" · ") : ""
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                visible: card.project !== null && card.project.folder !== ""
                spacing: Theme.space(1.5)
                Icon {
                    name: "folder"
                    size: Theme.iconXs
                    color: Theme.textTertiary
                }
                Text {
                    objectName: "homeContinueFolder"
                    Layout.fillWidth: true
                    text: card.project ? card.project.folder : ""
                    elide: Text.ElideMiddle
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }
            Item {
                Layout.fillHeight: true
                Layout.minimumHeight: Theme.space(2)
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                PillButton {
                    objectName: "homeContinueOpen"
                    height: Theme.controlHeightLarge
                    variant: "primary"
                    iconName: card.current ? "arrow-right" : "play"
                    text: card.current ? qsTr("Back to the project") : qsTr("Open")
                    tooltip: card.current ? qsTr("Close Home and go back to %1").arg(card.project ? card.project.name : "")
                                          : qsTr("Open %1").arg(card.project ? card.project.name : "")
                    onClicked: card.openProject()
                }
                PillButton {
                    objectName: "homeContinueReveal"
                    height: Theme.controlHeightLarge
                    variant: "ghost"
                    iconName: "folder"
                    text: Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Show in Explorer")
                    tooltip: qsTr("Show the project file in its folder")
                    onClicked: card.revealRequested(card.project.url)
                }
            }
        }
    }

    // ---- No recent project yet: one quiet line (Start, beside it, has New project and Open...) ----
    ColumnLayout {
        objectName: "homeContinueEmpty"
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: card.project === null
        spacing: Theme.space(3)

        Item { Layout.fillHeight: true }
        EchoMark {
            Layout.alignment: Qt.AlignHCenter
            size: Theme.space(12)
            mono: !Theme.dark || Theme.highContrast
        }
        Text {
            objectName: "homeContinueEmptyText"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Your latest project will be here.")
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Item { Layout.fillHeight: true }
    }
}
