#pragma once

// Embedded closed captions (0.6, #73): a caption track as the cc_data an export embeds - CEA-608 on one of its four
// channels (CC1 / CC2 on field 1, CC3 / CC4 on field 2) and CEA-708 in one of its services (1-63) - frame by frame.
//
//     ClosedCaptionOptions o;               // 608 CC1 + 708 service 1, pop-on, the track's placement and colours
//     ClosedCaptionData cc = buildClosedCaptions(track.cues, range, rate, frames, o);
//     cc.frame(k);                          // cc_count triplets for frame k
//
// Each frame carries cc_count triplets (ATSC A/53 cc_data, SMPTE 334-2 CDPs): 600 / fps of them (25 at 23.976, 24 at
// 25, 20 at 29.97, 12 at 50, 10 at 59.94), at most 31 - the 9600 bit/s of a DTVCC channel. 608 takes its byte pairs
// (interchange::cea608ForVideo, the SCC writer's timing) and a null pair on each field it does not use; 708 takes the
// rest as one DTVCC packet per frame; the leftover slots are 708 padding. The same triplets go into H.264 / HEVC SEI,
// MPEG-2 user data, an MXF SMPTE 436M track (FFmpeg wraps them in CDPs) and a QuickTime c608 track (its field 1 pairs).
//
// 708 (CEA-708-E): pop-on captions are loaded ahead of time into a hidden window - two windows (0 and 1) take turns,
// shown (DisplayWindows) on the caption's first frame and hidden at its end or by the next one; roll-up has one window
// of 2-4 rows that scrolls a line up (CR) per line; paint-on writes into a visible window as the caption starts. Windows
// sit at the bottom or top of the safe area, justified like the track's alignment, 32 columns wide at most (the 4:3
// safe title width, as 608); pens and window fills take the track's text and box colours on 708's 64-colour palette.
// Text is G0 / G1 (ASCII, Latin-1, music note) and the G2 symbols (quotes, ellipsis, bullet, TM, ...); other
// characters become '?'. A caption whose loading does not fit before its start is shown late; `late708` counts them.
//
// readCea708() reads 708 back from cc_data (a check and a test tool, independent of the writer's state).

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wabcut::exporter {

/// cc_data triplets per frame at `rate`: 600 / fps rounded, 1..31.
int ccCountForRate(Rate rate);

/// A CEA-608 byte pair (odd parity) written for CC1, as channel `channel` (1-4) writes it: CC2 / CC4 set the data
/// channel bit of control codes (10h-17h -> 18h-1Fh); CC3 / CC4 (field 2) use 15h / 1Dh for the miscellaneous control
/// codes (RCL, EOC, EDM, ...). Characters and null pairs are the same on every channel.
std::array<std::uint8_t, 2> cea608PairForChannel(std::array<std::uint8_t, 2> cc1, int channel);

struct ClosedCaptionOptions {
    bool cea608 = true;
    int channel608 = 1; ///< 1-4
    bool cea708 = true;
    int service708 = 1; ///< 1-63
    /// interchange::Cea608Display: 0 pop-on, 1-3 roll-up 2 / 3 / 4 rows, 4 paint-on (608 and 708 alike).
    std::uint8_t display = 0;
    CaptionPlacement placement = CaptionPlacement::Bottom; ///< Custom is written as Bottom
    CaptionAlign alignment = CaptionAlign::Center;
    /// 708: the text colour (fill), the background box (box, boxColor, boxOpacity) and the safe area.
    CaptionStyle style;
    std::string language; ///< ISO 639-1 (the 608 writer's)
};

/// The cc_data of every frame of an export, and what could not be carried exactly.
struct ClosedCaptionData {
    int ccCount = 0;
    std::int64_t frames = 0;
    std::vector<std::uint8_t> bytes; ///< frames x ccCount x 3
    int cues = 0;                    ///< captions in the range
    int late708 = 0;                 ///< 708 captions shown after their first frame (not enough bandwidth to load them)
    std::int64_t maxLateFrames = 0;
    /// Plain English: lines rewrapped, characters replaced, captions late (608 and 708).
    std::vector<std::string> notes;

    /// The triplets of frame `k` (empty out of range).
    std::span<const std::uint8_t> frame(std::int64_t k) const;
};

/// `cues` (sorted, the export's caption track with redactions applied) inside `range`, frame k = range.start + k frame
/// periods at `rate`, for `frames` frames. Nothing on and no cues: every frame carries padding only.
ClosedCaptionData buildClosedCaptions(const std::vector<CaptionCue>& cues, TimeRange range, Rate rate, std::int64_t frames,
                                      const ClosedCaptionOptions& options);

/// A 708 caption read back: the text of the visible windows (rows trimmed, joined by '\n'; windows by number) from the
/// frame it appeared to the frame it went (exclusive).
struct Cea708Caption {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
    std::string text;
    std::vector<int> windows; ///< the windows it was shown in
};

struct Cea708Reading {
    std::vector<Cea708Caption> captions;
    std::vector<int> windowsDefined; ///< every window number DefineWindow created, sorted
    std::vector<int> services;       ///< every service number seen in a service block, sorted
    int packets = 0;                 ///< complete DTVCC packets
    int errors = 0;                  ///< malformed packets, blocks or commands
};

/// Reads service `service` from per-frame cc_data triplets (cc_type 3 / 2: DTVCC packet start / data).
Cea708Reading readCea708(const std::vector<std::vector<std::uint8_t>>& ccData, int service);

} // namespace wabcut::exporter
