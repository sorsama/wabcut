#include "MxfDelivery.h"

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <utility>

namespace wabcut::exporter::detail {

namespace {

std::filesystem::path partialPath(const std::filesystem::path& file) {
    std::random_device rd;
    const std::uint64_t tag = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    std::filesystem::path name = file.stem();
    name += std::format(".wabcut-partial-{:016x}", tag);
    name += file.extension();
    return file.parent_path() / name;
}

std::string rateArgument(Rate rate) {
    return std::format("{}/{}", rate.num, rate.den);
}

} // namespace

std::filesystem::path opAtomAudioPath(const std::filesystem::path& video, int channel) {
    std::filesystem::path name = video.stem();
    name += std::format("_A{}", channel);
    name += video.extension().empty() ? std::filesystem::path(".mxf") : video.extension();
    return video.parent_path() / name;
}

int mxfMixChannels(int outputs, ChannelFormat master) {
    if (outputs <= 1) {
        return 1;
    }
    if (master == ChannelFormat::Surround71 && outputs >= 8) {
        return 8; // 0.7: the 7.1 mix over eight tracks
    }
    return outputs >= 6 && (master == ChannelFormat::Surround51 || master == ChannelFormat::Surround71) ? 6 : 2;
}

void configureMxfMuxer(Muxer& muxer, const MxfFileInfo& info) {
    if (!info.timecode.empty()) {
        muxer.setMetadata("timecode", info.timecode);
    }
    if (!info.clipName.empty()) {
        muxer.setMetadata("material_package_name", info.clipName);
    }
    if (!info.reelName.empty()) {
        muxer.setMetadata("reel_name", info.reelName);
    }
    muxer.setMetadata("company_name", "Wabcut");
    muxer.setMetadata("product_name", "Wabcut");
    if (info.height > 0) {
        // SMPTE 274M (1080-line) or 296M (720-line progressive) in the picture descriptor.
        if (info.height == 1080) {
            muxer.setFormatOption("signal_standard", "smpte274m");
        } else if (info.height == 720) {
            muxer.setFormatOption("signal_standard", "smpte296m");
        }
    }
}

MxfAudioOut::~MxfAudioOut() {
    for (Track& t : m_tracks) {
        if (t.own) {
            t.own->abort();
        }
        if (!t.temp.empty()) {
            std::error_code ec;
            std::filesystem::remove(t.temp, ec);
        }
    }
}

std::expected<std::unique_ptr<MxfAudioOut>, Error> MxfAudioOut::create(Container container, MxfAudioLayout layout,
                                                                       const AudioSettings& settings, int mixChannels,
                                                                       Muxer& main, const std::filesystem::path& video,
                                                                       const MxfFileInfo& info) {
    std::unique_ptr<MxfAudioOut> out(new MxfAudioOut);
    out->m_container = container;
    out->m_layout = container == Container::MxfOpAtom ? MxfAudioLayout::MonoTracks : layout;
    out->m_outputs = std::max(1, settings.channels);
    out->m_mixChannels = std::clamp(mixChannels, 1, out->m_outputs);
    out->m_bits = settings.codec == AudioCodec::Pcm16 ? 16 : 24;
    const bool mono = out->m_layout == MxfAudioLayout::MonoTracks;
    const int trackCount = mono ? out->m_outputs : 1;
    for (int t = 0; t < trackCount; ++t) {
        AudioSettings one = settings;
        one.channels = mono ? 1 : out->m_outputs;
        Track track;
        auto enc = AudioEncoder::open(one, false);
        if (!enc) {
            return std::unexpected(enc.error());
        }
        track.encoder = std::move(*enc);
        for (int c = 0; c < one.channels; ++c) {
            const int source = mono ? t : c;
            track.channels.push_back(source < out->m_mixChannels ? source : -1);
        }
        if (container == Container::MxfOpAtom) {
            track.final = opAtomAudioPath(video, t + 1);
            track.temp = partialPath(track.final);
            auto muxer = Muxer::create(Container::MxfOpAtom, track.temp);
            if (!muxer) {
                return std::unexpected(muxer.error());
            }
            track.own = std::make_unique<Muxer>(std::move(*muxer));
            MxfFileInfo audioInfo = info;
            audioInfo.height = 0;
            configureMxfMuxer(*track.own, audioInfo);
            // The audio file's edit rate is the picture's, so its timecode track counts the same frames.
            track.own->setFormatOption("mxf_audio_edit_rate", rateArgument(info.frameRate));
            track.muxer = track.own.get();
        } else {
            track.muxer = &main;
        }
        auto stream = track.muxer->addStream(track.encoder.context());
        if (!stream) {
            return std::unexpected(stream.error());
        }
        track.stream = *stream;
        out->m_encoderName = track.encoder.name();
        out->m_tracks.push_back(std::move(track));
    }
    return out;
}

std::expected<void, Error> MxfAudioOut::open() {
    for (Track& t : m_tracks) {
        if (t.own) {
            if (auto ok = t.own->open(); !ok) {
                return ok;
            }
        }
    }
    return {};
}

std::expected<void, Error> MxfAudioOut::encode(std::span<const float> mix) {
    const auto mixCh = static_cast<std::size_t>(m_mixChannels);
    const std::size_t frames = mix.size() / mixCh;
    for (Track& t : m_tracks) {
        const std::size_t ch = t.channels.size();
        t.buffer.assign(frames * ch, 0.0f);
        for (std::size_t c = 0; c < ch; ++c) {
            const int source = t.channels[c];
            if (source < 0) {
                continue;
            }
            for (std::size_t i = 0; i < frames; ++i) {
                t.buffer[i * ch + c] = mix[i * mixCh + static_cast<std::size_t>(source)];
            }
        }
        if (auto ok = t.encoder.encode(t.buffer, *t.muxer, t.stream); !ok) {
            return ok;
        }
    }
    return {};
}

std::expected<void, Error> MxfAudioOut::flush() {
    for (Track& t : m_tracks) {
        if (auto ok = t.encoder.flush(*t.muxer, t.stream); !ok) {
            return ok;
        }
    }
    return {};
}

std::expected<void, Error> MxfAudioOut::finishFiles() {
    for (Track& t : m_tracks) {
        if (!t.own) {
            continue;
        }
        if (auto ok = t.own->finish(); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return {};
}

std::expected<void, Error> commitFileSet(const std::vector<FileMove>& moves) {
    struct Placed {
        std::filesystem::path temp;
        std::filesystem::path final;
        std::filesystem::path backup; ///< the earlier file, moved aside (empty = there was none)
        bool placed = false;          ///< the new file has taken the name
    };
    std::vector<Placed> done;
    done.reserve(moves.size());
    const auto rollback = [&] {
        std::error_code ignored;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            if (it->placed) { // the new file goes back under its temporary name (its owner removes it)
                std::filesystem::rename(it->final, it->temp, ignored);
                if (ignored) {
                    std::filesystem::remove(it->final, ignored);
                }
            }
            if (!it->backup.empty()) {
                std::filesystem::rename(it->backup, it->final, ignored);
            }
        }
    };
    for (const FileMove& m : moves) {
        Placed p;
        p.temp = m.temp;
        p.final = m.final;
        std::error_code ec;
        if (std::filesystem::exists(m.final, ec)) {
            p.backup = partialPath(m.final); // ".wabcut-partial-" in the name: never mistaken for an export
            std::filesystem::rename(m.final, p.backup, ec);
            if (ec) {
                p.backup.clear();
                rollback();
                return std::unexpected(Error{ErrorCode::IoError, std::format("Cannot replace {} (is another program using it?): {}",
                                                                             pathToUtf8(m.final), ec.message())});
            }
        }
        done.push_back(std::move(p));
        std::filesystem::rename(m.temp, m.final, ec);
        if (ec) {
            rollback();
            return std::unexpected(Error{ErrorCode::IoError, std::format("Cannot move {} into place: {}",
                                                                         pathToUtf8(m.final), ec.message())});
        }
        done.back().placed = true;
    }
    std::error_code ignored;
    for (const Placed& p : done) {
        if (!p.backup.empty()) {
            std::filesystem::remove(p.backup, ignored);
        }
    }
    return {};
}

bool isWabcutMxf(const std::filesystem::path& file) {
    // configureMxfMuxer sets company_name and product_name to "Wabcut"; the MXF Identification set (in the header
    // partition, at the start of the file) holds them as UTF-16BE strings.
    static constexpr unsigned char kMark[] = {0x00, 'W', 0x00, 'a', 0x00, 'b', 0x00, 'c', 0x00, 'u', 0x00, 't'};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<char> head(256 * 1024);
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    const auto begin = reinterpret_cast<const unsigned char*>(head.data());
    return std::search(begin, begin + got, std::begin(kMark), std::end(kMark)) != begin + got;
}

std::vector<std::filesystem::path> removeStaleAudioFiles(const std::filesystem::path& video, int tracks,
                                                         const std::vector<std::filesystem::path>& keep,
                                                         std::vector<std::filesystem::path>* left) {
    constexpr int kMaxTracks = 64; // an MXF audio layout never goes beyond this
    std::vector<std::filesystem::path> removed;
    for (int n = std::max(tracks, 0) + 1; n <= kMaxTracks; ++n) {
        const std::filesystem::path file = opAtomAudioPath(video, n);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        const bool protectedFile = std::ranges::any_of(keep, [&](const std::filesystem::path& k) {
            std::error_code same;
            return std::filesystem::equivalent(file, k, same);
        });
        // 152 (review): only what Wabcut wrote. A camera original or another program's "<stem>_A<n>.mxf" next to the
        // output is not an earlier export's audio file, whatever its name.
        if (protectedFile || !isWabcutMxf(file)) {
            if (left) {
                left->push_back(file);
            }
            continue;
        }
        if (std::filesystem::remove(file, ec) && !ec) {
            removed.push_back(file);
        }
    }
    return removed;
}

std::expected<std::vector<std::filesystem::path>, Error> MxfAudioOut::commitFiles(const std::filesystem::path& pictureTemp,
                                                                                  const std::filesystem::path& picture) {
    std::vector<FileMove> moves;
    for (const Track& t : m_tracks) {
        if (t.own) {
            moves.push_back({t.temp, t.final}); // the audio first, the picture last
        }
    }
    moves.push_back({pictureTemp, picture});
    if (auto ok = commitFileSet(moves); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<std::filesystem::path> files;
    for (Track& t : m_tracks) {
        if (t.own) {
            t.temp.clear();
            files.push_back(t.final);
        }
    }
    return files;
}

std::string MxfAudioOut::describe() const {
    const bool mono = m_layout == MxfAudioLayout::MonoTracks;
    std::string text =
        m_container == Container::MxfOpAtom
            ? std::format("{} mono audio file{}", m_outputs, m_outputs == 1 ? "" : "s")
        : mono ? std::format("{} mono track{}", m_outputs, m_outputs == 1 ? "" : "s")
               : std::format("one {}-channel track", m_outputs);
    text += std::format(" · PCM {}-bit 48 kHz", m_bits);
    if (m_outputs > 1) {
        const std::string mix = m_mixChannels == 6 ? "5.1 mix" : m_mixChannels == 2 ? "stereo mix" : "mono mix";
        text += std::format(" · {} {}", m_mixChannels == 1 ? std::string("1") : std::format("1-{}", m_mixChannels), mix);
        if (m_outputs > m_mixChannels) {
            text += m_outputs == m_mixChannels + 1 ? std::format(", {} silent", m_outputs)
                                                   : std::format(", {}-{} silent", m_mixChannels + 1, m_outputs);
        }
    }
    return text;
}

std::expected<void, Error> CaptionTrackWriter::write(const std::vector<std::uint8_t>& bytes, std::int64_t pts,
                                                     std::int64_t duration) {
    PacketPtr packet(av_packet_alloc());
    if (!packet || av_new_packet(packet.get(), static_cast<int>(bytes.size())) < 0) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (captions)"});
    }
    std::copy(bytes.begin(), bytes.end(), packet->data);
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = duration;
    packet->flags |= AV_PKT_FLAG_KEY;
    return m_muxer->write(packet.get(), m_timeBase, m_stream);
}

std::expected<void, Error> CaptionTrackWriter::frame(std::int64_t k, std::span<const std::uint8_t> ccData) {
    if (!m_stream) {
        return {};
    }
    if (!m_quickTime) {
        return write(std::vector<std::uint8_t>(ccData.begin(), ccData.end()), k, 1);
    }
    std::vector<std::uint8_t> field1;
    for (std::size_t i = 0; i + 2 < ccData.size(); i += 3) {
        if (ccData[i] == 0xFC) { // valid, NTSC field 1
            field1.insert(field1.end(), ccData.begin() + static_cast<std::ptrdiff_t>(i),
                          ccData.begin() + static_cast<std::ptrdiff_t>(i + 3));
        }
    }
    if (field1.empty()) {
        return {}; // the sample before lasts one frame longer
    }
    if (m_pendingStart >= 0) {
        if (auto ok = write(m_pending, m_pendingStart, k - m_pendingStart); !ok) {
            return ok;
        }
    }
    m_pending = std::move(field1);
    m_pendingStart = k;
    return {};
}

std::expected<void, Error> CaptionTrackWriter::finish(std::int64_t frames) {
    if (!m_stream || !m_quickTime || m_pendingStart < 0) {
        return {};
    }
    const std::int64_t start = std::exchange(m_pendingStart, -1);
    return write(m_pending, start, std::max<std::int64_t>(1, frames - start));
}

std::string mxfMaterialUmid(const std::filesystem::path& file) {
    AVFormatContext* ctx = nullptr;
    const std::string path = pathToUtf8(file);
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
        return {};
    }
    std::string umid;
    if (const AVDictionaryEntry* e = av_dict_get(ctx->metadata, "material_package_umid", nullptr, 0)) {
        umid = e->value;
    }
    avformat_close_input(&ctx);
    return umid;
}

} // namespace wabcut::exporter::detail
