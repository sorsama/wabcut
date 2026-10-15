pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Library's search bar (AI-8): a field and the Smart toggle. Off, typing filters the library by clip name
// (matches()); on, the query goes to smart search (SmartSearch, what clips show and say) after a short pause, and
// turning it on indexes what is not indexed yet. Escape clears the field.
Item {
    id: root

    property alias query: field.text
    property alias smart: smartToggle.checked
    /// Smart results are showing (the Library swaps its grid for them).
    readonly property bool smartActive: smart && query.trim() !== ""

    /// Whether a library item named `name` passes the plain name filter.
    function matches(name: string): bool {
        const q = root.query.trim().toLowerCase()
        return root.smart || q === "" || name.toLowerCase().indexOf(q) >= 0
    }
    function focusField() {
        field.forceActiveFocus(Qt.ShortcutFocusReason)
    }

    implicitHeight: Theme.controlHeightSmall

    onSmartChanged: {
        if (root.smart) {
            SmartSearch.ensureIndexed()
            if (root.query.trim() !== "")
                SmartSearch.search(root.query)
        } else {
            SmartSearch.clear()
        }
    }

    // Search after a pause in typing, not on every key.
    Timer {
        id: debounce
        interval: 250
        onTriggered: SmartSearch.search(root.query)
    }

    TextField {
        id: field
        objectName: "librarySearchField"
        anchors.left: parent.left
        anchors.right: smartToggle.left
        anchors.rightMargin: Theme.space(2)
        anchors.verticalCenter: parent.verticalCenter
        implicitHeight: Theme.controlHeightSmall
        leftPadding: Theme.space(3) + Theme.iconXs + Theme.space(2)
        rightPadding: Theme.space(3)
        verticalAlignment: TextInput.AlignVCenter
        //: Library search field; with Smart on it searches what clips show and say
        placeholderText: root.smart ? qsTr("Describe a moment…") : qsTr("Search clips")
        color: Theme.textPrimary
        placeholderTextColor: Theme.textTertiary
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        selectByMouse: true
        Accessible.name: root.smart ? qsTr("Smart search") : qsTr("Search clips")

        onTextChanged: {
            if (!root.smart)
                return
            if (text.trim() === "")
                SmartSearch.clear()
            else
                debounce.restart()
        }
        onAccepted: {
            if (root.smart) {
                debounce.stop()
                SmartSearch.search(text)
            }
        }
        Keys.onEscapePressed: event => {
            if (text === "") {
                event.accepted = false
                return
            }
            text = ""
            event.accepted = true
        }
        // Editor shortcuts must not fire while typing here.
        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                                                           || event.key === Qt.Key_Delete || event.key === Qt.Key_Left
                                                           || event.key === Qt.Key_Right || event.key === Qt.Key_Home
                                                           || event.key === Qt.Key_End || event.key === Qt.Key_Escape

        background: Rectangle {
            radius: height / 2
            color: Theme.raised
            border.width: field.activeFocus ? Theme.focusRingWidth : 0
            border.color: Theme.focus

            Icon {
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(3)
                anchors.verticalCenter: parent.verticalCenter
                size: Theme.iconXs
                name: root.smart ? "sparkles" : "search"
                color: root.smart ? Theme.accentStroke : Theme.textTertiary
            }
        }
    }

    Toggle {
        id: smartToggle
        objectName: "smartSearchToggle"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        //: Library search: search what clips show and say (an on-device AI model) instead of their names
        text: qsTr("Smart")
        tooltip: qsTr("Smart search: find moments by what they show or say, on this computer")
    }
}
