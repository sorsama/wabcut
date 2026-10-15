pragma ComponentBehavior: Bound
import QtQuick

// Remix (M5m; bridge `EssentialSound`; Premiere's Remix tool): the music clip's remix to a new length, previewed before
// it is applied. The joins are drawn on the timeline (a marker at each, the new length as a ghost); here: the target and
// the result, each join with how seamless it is (green from 80 %, amber from 35 %, red below), the warnings, and
// Apply / Cancel. The menu fits the music to the video or renders the remix as one take instead of cuts.
// Open it with openAt(item, point) after EssentialSound.preview(); closing it without Apply drops the preview.
Popover {
    id: popover
    objectName: "remixPopover"

    /// Shows the popover at `point` of `item` (the timeline's lanes, where the clip's end was dragged).
    function openAt(item, point) {
        parent = item
        x = Math.max(0, Math.min(point.x, item.width - width))
        y = Math.max(0, Math.min(point.y, item.height - height))
        open()
    }

    title: qsTr("Remix")
    width: Theme.space(88)
    onClosed: {
        if ((EssentialSound.remix.clip ?? "") !== "")
            EssentialSound.cancel()
    }

    RemixSummary {
        width: popover.availableWidth
        onFinished: popover.close()
    }
}
