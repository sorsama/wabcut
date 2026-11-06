# VR / 360 video (0.7)

Wabcut edits equirectangular 360 and 180 degree (VR180) video, mono or stereoscopic: it reads the spherical metadata of a file,
lets you look around in the Program and Source Monitors, turns a scene with **VR Reorient**, and marks the export as VR video
so players and YouTube recognise it.

## What a picture is

A VR picture is an **equirectangular** frame: the whole sphere on a flat rectangle (a 360 eye is 2:1, such as 3840 x 1920) or
the front half of it (a 180 eye is 1:1). A **stereoscopic** frame holds both eyes, the left eye first: **Top - Bottom**
(left eye on top, 3840 x 3840 for two 360 eyes) or **Side by Side** (left eye on the left).

| Where | What it says |
|---|---|
| The file | Spherical Video V1 (XML) and V2 (`sv3d` / `st3d` boxes) in MP4 and MOV, Matroska's Projection and StereoMode. Wabcut reads them when it probes a file and shows what it found in **Interpret Footage**. Cube maps, fisheye and cropped projections are not drawn: such a file is treated as a flat picture. |
| Interpret Footage | **Clip > Modify > Interpret Footage... > VR video**: *Use VR Settings from File*, *Flat Video (not VR)* (when a file claims 360 wrongly), *Equirectangular 360* or *Equirectangular 180 (VR180)*, and the stereoscopic layout. Command: `media.interpretFootage` with `vr`; `vr.info` reads it. |
| The sequence | **Sequence > Sequence Settings... > VR video** (same choices). Make a sequence from a 360 clip (**New Sequence From Clip**) and it is a VR sequence of the clip's format. The dialog says when the frame is not the shape of the picture. Command: `sequence.setSettings` with `vr`. |

The settings are saved in the project only when set (an older Wabcut opens the project and shows a flat picture; **Save for an
Older Version** says so).

## Look around: the VR viewer

With a VR sequence open, **Program Monitor wrench > VR Video View** turns the monitor into a viewer (the same menu item is in
the Source Monitor's wrench for a 360 item). The picture is then the flat view of someone standing in the middle of the
sphere, drawn on the GPU from the sequence's full-size frame.

| Do | Result |
|---|---|
| Drag | Look around (pull the scene the way you would pull a photograph) |
| Scroll | Zoom: the field of view between 10 and 150 degrees (90 to start) |
| Arrow keys, + and - | Look around and zoom from the keyboard (the viewer takes focus when you click it) |
| Home, double-click, **Reset View** | Straight ahead, level, 90 degrees |

A badge shows the yaw, pitch and field of view. A stereoscopic frame is viewed through the left eye. Guides, rulers and the
Motion box measure the flat frame, so they are covered while the viewer is on; turn it off to get them back. The viewer is for
looking: it does not change the project or the export (an export is always the full frame).

## VR Reorient

**Effects > Immersive Video > VR Reorient** (`wabcut.vr.reorient`) turns the sphere: **Yaw** (to the right), **Pitch** (up) and
**Roll** (a tilt to the right), in degrees, all keyframable, so a different part of the scene lands straight ahead (or a shot's
horizon is levelled, or a scene follows the action). Set **Projection** and **Stereoscopic Layout** to the picture's; each eye of
a stereoscopic picture turns alike. `vr.reorient` adds the effect to clips with the format of the clip's footage (or the
sequence).

## Exporting

Exporting a VR sequence to **MP4**, **Matroska (MKV)** or **WebM** writes the spherical metadata (`sv3d` and `st3d` boxes in MP4,
Projection and StereoMode in Matroska), so YouTube, Facebook, headsets and Wabcut itself open the file as 360 video. VR180 is
written as the front half of the sphere (the `equi` box's quarter crops). Use a frame size that suits the projection (3840 x 1920
or larger for 360; YouTube asks for H.264 or VP9). QuickTime (MOV) and the other formats have no such metadata in Wabcut's
muxers: the file is exported without it.

## Not in this version

* A flat view of a 360 clip with a keyframed look direction (a reframe) for a 16:9 sequence: drag the viewer for a look; the
  effect needs the full-resolution frame, which a clip effect does not have (it works on the clip as placed on the canvas).
* VR transitions, cube maps and other projections, reading the camera's initial view (heading, pitch, roll) and spatial audio
  (ambisonics) metadata.
* Head-mounted displays.
