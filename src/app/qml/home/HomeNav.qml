pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Home screen's left rail (design language v3): an island with the logo, one pill per section of the page (a hue
// dot and its name; the section in view is lit, a click scrolls to it), and at the bottom where the work lives with a
// round Settings button. With the custom window frame the logo area moves the window.
Rectangle {
    id: root
    objectName: "homeNav"

    /// [{ key, label, hue }]: the sections, top to bottom.
    property var sections: []
    property string current: ""
    property bool dragsWindow: false

    signal activated(string key)
    signal settingsRequested()

    radius: Theme.radiusPanel
    color: Theme.panel

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Home sections")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(3)
        anchors.topMargin: Theme.space(4)
        spacing: Theme.space(4.5)

        Item {
            Layout.fillWidth: true
            implicitHeight: logo.implicitHeight

            BrandLogo {
                id: logo
                x: Theme.space(1.5)
                markSize: Theme.space(8)
                showVersion: false
            }
            // The window moves by its logo area (custom frame only).
            Item {
                anchors.fill: parent
                enabled: root.dragsWindow
                TapHandler {
                    gesturePolicy: TapHandler.DragThreshold
                    onDoubleTapped: {
                        const w = root.Window.window
                        if (w)
                            w.visibility === Window.Maximized ? w.showNormal() : w.showMaximized()
                    }
                }
                DragHandler {
                    target: null
                    onActiveChanged: {
                        if (active && root.Window.window)
                            root.Window.window.startSystemMove()
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space(0.5)

            Repeater {
                model: root.sections
                delegate: AbstractButton {
                    id: entry
                    required property var modelData
                    readonly property bool selected: root.current === modelData.key
                    objectName: "homeNav_" + modelData.key
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeightLarge
                    leftPadding: Theme.space(3.5)
                    rightPadding: Theme.space(3.5)
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.name: modelData.label
                    Accessible.description: qsTr("Scroll to %1").arg(modelData.label)
                    Accessible.selected: selected
                    onClicked: root.activated(modelData.key)

                    background: Rectangle {
                        radius: height / 2
                        color: entry.selected ? Theme.hover : entry.hovered ? Theme.raised : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    }
                    contentItem: RowLayout {
                        spacing: Theme.space(3)
                        Rectangle {
                            implicitWidth: Theme.dotSize
                            implicitHeight: Theme.dotSize
                            radius: Theme.dotSize / 2
                            color: Theme.hue(entry.modelData.hue)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: entry.modelData.label
                            elide: Text.ElideRight
                            color: entry.selected || entry.hovered ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightLabel
                            Behavior on color { ColorAnimation { duration: Theme.durFast } }
                        }
                    }
                    FocusRing {
                        targetRadius: entry.height / 2
                        visible: entry.visualFocus
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space(1.5)
            spacing: Theme.space(2)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Local workspace")
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Theme.weightLabel
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Wabcut %1").arg(Qt.application.version)
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
            IconButton {
                objectName: "homeSettingsButton"
                variant: "secondary"
                iconName: "settings"
                tooltip: qsTr("Settings")
                onClicked: root.settingsRequested()
            }
        }
    }
}
