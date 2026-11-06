pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// File > Cloud Project and Review Links… (0.7, docs/cloud.md): keep a project on storage you own, share a review copy with a page
// that collects comments, bring the comments back. Three pages: Storage (the places you keep things: a folder a sync service
// mirrors, WebDAV, an S3-compatible bucket - passwords and keys go to the system's key store), Cloud project (upload, open, sync,
// the editing lock, the revisions) and Review links (make the review copy, see its size, publish, revoke, read the comments back).
// Nothing is uploaded before a button that says so is pressed, and offline mode or the project's no-cloud lock switch every upload
// off with the reason. Bridge: Cloud (CloudBridge).
WabcutDialog {
    id: dialog
    objectName: "cloudDialog"

    /// 0 Storage, 1 Cloud project, 2 Review links.
    property int page: 0

    title: qsTr("Cloud Project and Review Links")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(190), Theme.space(190))

    function showPage(index: int) {
        dialog.page = index
    }
    onOpened: Cloud.refreshCloud()

    // The body scrolls when the window is too short for it (the smallest window is 1100 x 700); the buttons stay in view.
    Flickable {
        id: scroller
        objectName: "cloudScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(bodyColumn.implicitHeight,
                                         Math.max(Theme.space(40), (dialog.parent ? dialog.parent.height : Theme.space(170)) - Theme.space(64)))
        contentWidth: width
        contentHeight: bodyColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: bodyColumn
            width: scroller.width - Theme.space(3)
            spacing: Theme.space(3)

            PillTabs {
                id: tabs
                objectName: "cloudTabs"
                Layout.fillWidth: true
                compact: true
                Accessible.name: qsTr("Cloud page")
                model: [qsTr("Storage"), qsTr("Cloud project"), qsTr("Review links")]
                currentIndex: dialog.page
                onActivated: index => dialog.page = index
            }

            // What is running, and its Cancel.
            RowLayout {
                objectName: "cloudJob"
                Layout.fillWidth: true
                visible: Cloud.busy && Cloud.phase !== ""
                spacing: Theme.space(2)
                Text {
                    objectName: "cloudPhase"
                    text: Cloud.phase
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                    Layout.maximumWidth: Theme.space(70)
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Theme.space(1.5)
                    radius: height / 2
                    color: Theme.well
                    Rectangle {
                        width: parent.width * (Cloud.progress < 0 ? 0.25 : Math.min(1, Cloud.progress))
                        height: parent.height
                        radius: height / 2
                        color: Theme.accentFill
                    }
                }
                PillButton {
                    objectName: "cloudCancel"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Cancel")
                    onClicked: Cloud.cancelJob()
                }
            }
            Text {
                objectName: "cloudMessage"
                Layout.fillWidth: true
                visible: Cloud.message !== ""
                text: Cloud.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Cloud.messageIsError ? Theme.warning : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                Accessible.role: Accessible.StaticText
            }

            StackLayout {
                Layout.fillWidth: true
                currentIndex: dialog.page
                CloudStoragePage {
                    objectName: "cloudStoragePage"
                    onLocationSaved: id => dialog.page = Cloud.locations.find(l => l.id === id)?.purpose === "reviews" ? 2 : 1
                }
                CloudProjectPage {
                    objectName: "cloudProjectPage"
                    onNeedStorage: dialog.page = 0
                }
                ReviewLinksPage {
                    objectName: "cloudReviewPage"
                    onNeedStorage: dialog.page = 0
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "cloudClose"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
