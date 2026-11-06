# Capture from a camera, a capture card or a tape deck (0.7)

**File > Capture…** records live video straight into a project: a webcam, a capture card (HDMI, SDI, composite and S-video
grabbers for VHS, U-matic and other tape), or a camera or deck the system exposes as a video capture device. It is Premiere's
old Capture panel, adapted: pick a device and one of its formats, watch the live picture, press **Record** and **Stop**, and the
finished file is in the bin you chose, as one undo step named **Capture**.

Code: `src/capture` (the device layer, the recorder, the repair of an interrupted capture), `CaptureWriter.h` in `src/export`
(the file side), `CaptureBridge` and `CaptureDialog.qml` in `src/app`. Tests: `tests/capture`, `tests/app/test_capture_ui.cpp`.

| | |
|---|---|
| Platforms | **Windows** in this version, through Media Foundation. Linux (V4L2) and macOS (AVFoundation) are a later step: the panel opens and says so. |
| Devices | Everything Windows lists as a video capture device: UVC webcams, capture cards with a Windows driver, and what a DV deck or camera over FireWire or USB exposes *if its driver does* (see [Tape](#tape)). |
| Sound | Any sound input (the capture card's own, a microphone, line in), chosen separately: capture cards present their audio as an ordinary sound input. Recorded as uncompressed 16-bit stereo, 48 kHz. |
| File | `<clip name>.mkv` in the project's Capture folder (Matroska, which survives the program stopping), imported into the project when you press Stop. |

## Using it

1. **File > Capture…** opens the panel beside the editor (it is not modal; the editor stays usable). The devices are listed;
   **nothing is opened**.
2. **Video device**: pick one. Windows opens it for a moment to read its formats, so the camera light may blink.
3. **Format**: picture size, frame rate and what the device sends (`MJPG`, `YUY2`, `NV12`, `H264`...), the largest first. The
   format used last time with that device is chosen again. Interlaced formats (a tape deck) are marked and captured as full
   frames with both fields woven; deinterlace in the editor if the footage needs it.
4. **Sound**: the switch turns sound off; the menu picks the input (the system default is first).
5. **Quality**: *High quality (H.264)*, *Compact (H.264)* or *Every frame whole (ProRes 422)* (below). The estimate under the
   picture shows the room the drive has at that setting ("212 GB free on Data: room for about 9 h at this setting"), and
   the real rate once the capture has run five seconds.
6. **Clip name** and **Add to bin**: the file's name and the Library bin it goes into (the bin the Library shows is chosen).
7. **Start preview** opens the device and shows the live picture and the sound level. The top bar shows a red **Camera** pill
   for as long as a device is open.
8. **Record** starts the file. The panel shows the time, the frames dropped, the size written and the encoder; **Stop**
   finishes the file and adds it to the project (one undo step, **Capture**). The preview carries on for the next take.
9. **Close** (or Escape) closes the panel and the device. A capture that is running cannot be closed on: Stop it first.

The camera and the microphone open only when you press **Start preview** or **Record**, never when the panel opens or a
device is picked. Recording is not a command: no agent, script or MCP client can start it.

### Windows says no

If Windows blocks the camera or the microphone for desktop apps, the panel says so and offers **Open camera settings** /
**Open microphone settings**. The switches are in **Settings > Privacy & security > Camera** ("Let desktop apps access your
camera") and **> Microphone**. A device another program is using (a video call, a recording app) is reported as such: close
that program and try again.

## How it is written

| Preset | Video | Use | About |
|---|---|---|---|
| High quality (H.264) | H.264, constant quality 18, a key frame each second | The default: a good picture in files any editor reads | 1.6 MB/s at 1080p30 |
| Compact (H.264) | H.264, constant quality 24 | Long recordings that will not be graded | 0.8 MB/s at 1080p30 |
| Every frame whole (ProRes 422) | ProRes 422 (Standard), 10-bit 4:2:2, every frame stands alone | Tape, and material that will be cut hard or graded | 19 MB/s at 1080p30, a sixth of that at standard definition |

- **Encoders.** The export's choice: the graphics card's encoder first (NVENC, Quick Sync, AMF), probed with a test frame, then
  x264 at its `veryfast` preset so a live capture keeps up. The panel names the encoder in use.
- **Constant frame rate.** Picture 0 is the first one the device delivers; picture k is shown at k / rate. A slot the device
  left empty (it skipped a picture, or the computer was too slow to take one) is filled with the previous picture and counted
  under **frames dropped**; a picture that arrives for a slot already filled is not written. Rates a device reports as
  `10000000/333667` are put on the standard 29.97 (23.976, 59.94, 24, 25, 30, 50, 60...).
- **Sound in step with the picture.** The first sound is cut or padded with silence so it starts with the first picture; sound
  the device lost is replaced by silence of the same length; a sound device whose clock runs a little fast or slow against the
  computer's is corrected in steps when it is 50 ms out.
- **Colour and shape.** Standard definition (under 720 lines) is tagged Rec.601 (625-line PAL: BT.470 B/G), HD Rec.709, both
  limited range. The pixel shape the device reports (DV 4:3 is 10:11) is written into the file.
- **Memory.** About a second and a half of pictures wait for the encoder (at most 128 pictures or 512 MB); when it cannot keep
  up, pictures are dropped and counted, never waited for.

## Safe against a crash

While a capture is written, the file is a Matroska file with a new cluster each second, every packet flushed, and a small
marker next to it (`<clip name>.mkv.capture`). If the program or the computer stops:

- **Stop** (or a normal close) finishes the file and removes the marker.
- **Closing the project, opening or starting another one, or quitting** during a capture finishes the file and keeps the marker; the next time the project opens,
  a toast says "1 capture did not reach this project", and **File > Capture…** lists it with **Recover and add** and **Delete**.
- **A crash, a power cut, a kill**: what reached the disk (all but the last second) is kept. **Recover and add** copies the
  pictures and sound into a clean file in place of the broken one (index, duration) and adds it to the bin you choose, as one
  undo step. A capture with no picture in it is removed.
- A device that disappears (unplugged) or a drive that has less than 300 MB left stops the capture the same way, finished, and
  says why; a capture is not started with less than 500 MB free.

## Tape

Wabcut can capture from a tape deck only through what the operating system exposes as a **video capture device**:

- **Analog tape (VHS, Video8, U-matic, Betacam)** through a composite, S-video or component capture card or USB grabber: the card
  appears as a capture device (often with `YUY2` at 720 x 480 at 29.97 or 720 x 576 at 25) and its sound as an ordinary sound
  input. Pick that sound input in the panel.
- **DV and HDV over FireWire (IEEE 1394)**: Windows no longer ships a FireWire video capture driver, so a DV camera or deck
  appears only if a driver of the maker provides one (some do, as a DirectShow or Media Foundation capture device); otherwise it
  is not seen here. Wabcut does not control the deck: **there is no remote transport** (play, rewind, stop, timecode logging and
  batch capture of a tape's marked clips are not part of this version). Press Play on the deck, then Record in the panel.
- **SDI and HDMI capture cards** (Blackmagic, Elgato, AVerMedia, Magewell...) that expose a standard capture device work as
  webcams do; cards that need their own SDK are not seen.

## Not in this version

- Linux (V4L2) and macOS (AVFoundation) devices.
- Device control (deck transport, timecode, batch capture), FireWire without a driver that exposes a capture device.
- Changing the picture format while a capture runs (a capture card whose input signal changes size mid-capture drops those
  pictures; stop and start again).
- A hardware-accelerated or Direct3D-surface path for the live picture (it is converted on the CPU at about 30 pictures a second
  from a small copy), and 10-bit capture formats beyond `P010` as the device delivers them.
- HDR capture (the picture is tagged SDR).

## Testing

`tests/capture` runs the device layer, the recorder and the repair without any hardware, over a fake backend (a moving test
picture with its number as a barcode, a tone): the grid of pictures and the repeated slots, sound alignment, the drift
correction, the bounded queue, the crash repair, the privacy behaviour (nothing opens until asked; the refusals), an unplugged
device. The device test lists whatever is connected to the computer and is skipped when nothing is.
`tests/app/test_capture_ui.cpp` drives the panel over the fake backend, screenshots included.
