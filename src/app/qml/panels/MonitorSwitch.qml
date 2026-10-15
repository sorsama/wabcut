import QtQuick

// Source / Program switch in a monitor's header (M5h): narrow windows show one monitor at a time.
PillTabs {
    id: root
    objectName: "monitorSwitch"

    // "source" or "program"
    property string current: "program"
    signal switched(string monitor)

    compact: true
    implicitWidth: Theme.space(40)
    model: [qsTr("Source"), qsTr("Program")]
    currentIndex: current === "source" ? 0 : 1
    onActivated: index => root.switched(index === 0 ? "source" : "program")
}
