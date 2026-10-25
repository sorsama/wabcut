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

    Text {
        objectName: "aboutVersion"
        Layout.fillWidth: true
        //: About Wabcut: %1 is the version (0.4.0)
        text: qsTr("Version %1").arg(Qt.application.version)
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
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
