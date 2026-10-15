pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Continue (design language v3): the project opened most recently that is still there (Home.recent), large: its
// thumbnail, its name, when it was opened and its format, and Open. When it is the project open behind Home, Open goes
// back to it (`returnRequested`) instead of loading it again. With no recent project it offers Open….
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

    signal openRequested(url file)      // an empty url: choose a file
    signal returnRequested()

    function samePath(a: string, b: string): bool {
        const norm = s => String(s).replace(/\\/g, "/").toLowerCase()
        return norm(a) === norm(b)
    }
    function openProject() {
        if (card.project === null)
            card.openRequested("")
        else if (card.current)
            card.returnRequested()
        else
            card.openRequested(card.project.url)
    }

    Item {
        id: row
        Layout.fillWidth: true

        readonly property bool wide: width >= Theme.space(120)
        readonly property int gutter: Theme.space(5)
        readonly property real thumbWidth: wide ? Math.round((width - gutter) * 0.56) : width

        implicitHeight: wide ? Math.max(thumb.height, info.implicitHeight)
                             : thumb.height + Theme.space(3) + info.implicitHeight

        ProjectThumb {
            id: thumb
            objectName: "homeContinueThumb"
            width: row.thumbWidth
            height: Math.round(thumb.width * 9 / 16)
            cornerRadius: Theme.radiusCard
            source: card.project ? card.project.thumbnail : ""
            hueName: "violet"
            seed: 1
        }

        ColumnLayout {
            id: info
            x: row.wide ? thumb.width + row.gutter : 0
            y: row.wide ? 0 : thumb.height + Theme.space(3)
            width: row.wide ? row.width - x : row.width
            height: row.wide ? Math.max(thumb.height, implicitHeight) : implicitHeight
            spacing: Theme.space(2)

            Text {
                Layout.fillWidth: true
                text: card.project ? qsTr("Continue") : qsTr("Start here")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMicro + 1
                font.weight: Theme.weightTitle
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.microTracking * 1.5
            }
            Text {
                objectName: "homeContinueName"
                Layout.fillWidth: true
                text: card.project ? card.project.name : qsTr("No recent project yet")
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textDisplay + 2
                font.weight: Theme.weightTitle
                font.letterSpacing: -0.02 * (Theme.textDisplay + 2)
            }
            Text {
                Layout.fillWidth: true
                text: card.project ? [card.project.openedText, card.project.format].filter(s => s !== "").join(" · ")
                                   : qsTr("Projects you open or save appear here. Start a new one, import a timeline or open a project file.")
                wrapMode: card.project ? Text.NoWrap : Text.Wrap
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: card.project ? Theme.fontMono : Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Text {
                Layout.fillWidth: true
                visible: card.project !== null && card.project.folder !== ""
                text: card.project ? card.project.folder : ""
                elide: Text.ElideMiddle
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Item {
                Layout.fillHeight: true
                Layout.minimumHeight: Theme.space(2)
            }
            PillButton {
                objectName: "homeContinueOpen"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeightLarge
                variant: card.project ? "primary" : "secondary"
                iconName: card.project ? "play" : "folder-open"
                text: !card.project ? qsTr("Open a project…") : card.current ? qsTr("Back to the project") : qsTr("Open")
                tooltip: !card.project ? qsTr("Open a project file")
                       : card.current ? qsTr("Close Home and go back to %1").arg(card.project.name)
                       : qsTr("Open %1").arg(card.project.name)
                onClicked: card.openProject()
            }
        }
    }
}
