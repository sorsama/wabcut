import QtQuick

// SliceRect: v2's signature shape, "the slice": a rectangle whose top-right corner (top-left when `mirrored`) is cut by
// a straight 12° slice `depth` px deep, the other corners rounded by `radius`, filled with `color` or the beam (`beam`).
// Design language v3 sets the depth to 0 (Theme.sliceDepth), so it draws a plain rounded rectangle (a pill once the
// radius reaches half its height); the API stays for its callers. Drawn by SliceShape (C++): vertex-antialiased, cached
// until a property or the size changes.
SliceShape {
    id: root

    kind: SliceShape.Slice
    color: Theme.accentFill
    radius: Theme.radiusControl
    depth: Theme.sliceDepth
    angle: Theme.sliceAngle
    beamStart: Theme.beamStart
    beamMid: Theme.beamMid
    beamEnd: Theme.beamEnd
}
