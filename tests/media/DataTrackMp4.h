#pragma once

// A minimal MP4 with ONE data track (0.7, the camera's gyro): the sample entry's four characters ("gpmd", "rtmd", "djmd"),
// a handler name, and the samples as given, each lasting `delta` ticks of `timescale`. Enough for FFmpeg's demuxer to list
// the track and hand out its packets, so the data-track reader and the gyro service are tested on a real container.

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace wabcut::test {

using Mp4Bytes = std::vector<std::uint8_t>;

namespace mp4 {

inline void u16(Mp4Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void u32(Mp4Bytes& b, std::uint32_t v) {
    u16(b, v >> 16);
    u16(b, v & 0xFFFF);
}
inline void zeros(Mp4Bytes& b, std::size_t n) { b.insert(b.end(), n, 0); }
inline void text(Mp4Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

inline Mp4Bytes box(const std::string& type, const Mp4Bytes& payload) {
    Mp4Bytes out;
    u32(out, static_cast<std::uint32_t>(8 + payload.size()));
    text(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
/// A box that starts with version 0 and flags.
inline Mp4Bytes fullBox(const std::string& type, const Mp4Bytes& payload, std::uint32_t flags = 0) {
    Mp4Bytes body;
    u32(body, flags);
    body.insert(body.end(), payload.begin(), payload.end());
    return box(type, body);
}
inline Mp4Bytes join(std::initializer_list<Mp4Bytes> parts) {
    Mp4Bytes out;
    for (const Mp4Bytes& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

} // namespace mp4

inline Mp4Bytes dataTrackMp4(const std::string& sampleEntry, const std::string& handler, const std::vector<Mp4Bytes>& samples,
                             std::uint32_t timescale = 1000, std::uint32_t delta = 1000) {
    using namespace mp4;
    const auto count = static_cast<std::uint32_t>(samples.size());
    const Mp4Bytes ftyp = box("ftyp", join({[] { Mp4Bytes b; text(b, "mp42"); u32(b, 0); text(b, "mp42isom"); return b; }()}));
    const auto build = [&](std::uint32_t chunkOffset) {
        Mp4Bytes mvhd;
        u32(mvhd, 0); u32(mvhd, 0); u32(mvhd, timescale); u32(mvhd, count * delta); u32(mvhd, 0x00010000); u16(mvhd, 0x0100);
        zeros(mvhd, 10);
        for (const std::uint32_t m : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) {
            u32(mvhd, m);
        }
        zeros(mvhd, 24);
        u32(mvhd, 2);
        Mp4Bytes tkhd;
        u32(tkhd, 0); u32(tkhd, 0); u32(tkhd, 1); u32(tkhd, 0); u32(tkhd, count * delta);
        zeros(tkhd, 8); u16(tkhd, 0); u16(tkhd, 0); u16(tkhd, 0); u16(tkhd, 0);
        for (const std::uint32_t m : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) {
            u32(tkhd, m);
        }
        u32(tkhd, 0); u32(tkhd, 0);
        Mp4Bytes mdhd;
        u32(mdhd, 0); u32(mdhd, 0); u32(mdhd, timescale); u32(mdhd, count * delta); u16(mdhd, 0x55C4); u16(mdhd, 0);
        Mp4Bytes hdlr;
        u32(hdlr, 0); text(hdlr, "meta"); zeros(hdlr, 12); text(hdlr, handler); hdlr.push_back(0);
        Mp4Bytes dref;
        u32(dref, 1);
        const Mp4Bytes url = fullBox("url ", {}, 1);
        dref.insert(dref.end(), url.begin(), url.end());
        Mp4Bytes entry;
        zeros(entry, 6); u16(entry, 1);
        Mp4Bytes stsd;
        u32(stsd, 1);
        const Mp4Bytes sampleBox = box(sampleEntry, entry);
        stsd.insert(stsd.end(), sampleBox.begin(), sampleBox.end());
        Mp4Bytes stts;
        u32(stts, 1); u32(stts, count); u32(stts, delta);
        Mp4Bytes stsc;
        u32(stsc, 1); u32(stsc, 1); u32(stsc, count); u32(stsc, 1);
        Mp4Bytes stsz;
        u32(stsz, 0); u32(stsz, count);
        for (const Mp4Bytes& s : samples) {
            u32(stsz, static_cast<std::uint32_t>(s.size()));
        }
        Mp4Bytes stco;
        u32(stco, 1); u32(stco, chunkOffset);
        const Mp4Bytes stbl = box("stbl", join({fullBox("stsd", stsd), fullBox("stts", stts), fullBox("stsc", stsc), fullBox("stsz", stsz), fullBox("stco", stco)}));
        const Mp4Bytes minf = box("minf", join({fullBox("nmhd", {}), box("dinf", fullBox("dref", dref)), stbl}));
        const Mp4Bytes mdia = box("mdia", join({fullBox("mdhd", mdhd), fullBox("hdlr", hdlr), minf}));
        return box("moov", join({fullBox("mvhd", mvhd), box("trak", join({fullBox("tkhd", tkhd, 3), mdia}))}));
    };
    const Mp4Bytes sizing = build(0);
    const auto offset = static_cast<std::uint32_t>(ftyp.size() + sizing.size() + 8);
    Mp4Bytes mdat;
    for (const Mp4Bytes& s : samples) {
        mdat.insert(mdat.end(), s.begin(), s.end());
    }
    return join({ftyp, build(offset), box("mdat", mdat)});
}

inline bool writeBytes(const std::string& path, const Mp4Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace wabcut::test
