pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Help > About Wabcut: the logo with the version, what Wabcut is, its licence, and the website and issue tracker (the
// links Home.links has). Static and offline.
WabcutDialog {
    id: dialog
    objectName: "aboutDialog"

    title: qsTr("About Wabcut")

    // The logo, large, with the version.
    BrandLogo {
        objectName: "aboutLogo"
        Layout.alignment: Qt.AlignLeft
        markSize: Theme.space(12)
        showVersion: false
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Text {
            objectName: "aboutVersion"
            Layout.fillWidth: true
            //: About Wabcut: %1 is the version (0.5.0)
            text: qsTr("Version %1").arg(Qt.application.version)
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            elide: Text.ElideRight
        }
        // 0.7 (card 016): the version line on the clipboard, for a message or a bug report.
        PillButton {
            objectName: "aboutCopyVersion"
            compact: true
            variant: "ghost"
            iconName: "copy"
            text: copied.running ? qsTr("Copied") : qsTr("Copy")
            tooltip: qsTr("Copy the version")
            Accessible.name: qsTr("Copy the version")
            onClicked: {
                CrashNotice.copyText("Wabcut " + Qt.application.version)
                copied.restart()
            }
            Timer {
                id: copied
                interval: 1500
            }
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("A video editor for gaming videos, video essays and documentaries, with the keys and panels of Premiere Pro.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("Free software under the GNU General Public License, version 3. Manrope and JetBrains Mono under the SIL Open Font License; icons from Boxicons (MIT).")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // 0.7 (card 103): what this build does with the newer camera codecs, asked of the FFmpeg inside it.
    Text {
        Layout.fillWidth: true
        text: qsTr("Newer camera codecs")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }
    Repeater {
        objectName: "aboutCodecSupport"
        model: Library.codecSupport()
        delegate: ColumnLayout {
            id: codec
            required property var modelData
            required property int index
            objectName: "aboutCodec_" + codec.modelData.id
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: codec.modelData.name
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: codec.modelData.text
                color: codec.modelData.plays ? Theme.textTertiary : Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                wrapMode: Text.Wrap
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "aboutWebsiteButton"
            text: qsTr("Website")
            iconName: "external-link"
            onClicked: Qt.openUrlExternally(Home.websiteUrl)
        },
        PillButton {
            objectName: "aboutCloseButton"
            text: qsTr("Close")
            variant: "primary"
            onClicked: dialog.close()
        }
    ]
}
