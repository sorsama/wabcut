// AAF export for audio post (AudioPostExport.h): the plan (AudioPost.h) written as an AAF Edit Protocol composition.
//
//   Header (Edit Protocol) -> ContentStorage
//     CompositionMob "<sequence>" (TopLevel)
//       slot 1  Timecode (start timecode, frames)
//       V1..Vn  Sequence of SourceClip / Filler (frames) -> MasterMob -> SourceMob (CDCI, NetworkLocator)
//       A1..An  [MonoAudioPan [MonoAudioGain [Sequence of Filler / SourceClip / OperationGroup(MonoAudioGain,
//               SourceClip) / Transition(MonoAudioDissolve)]]] (samples) -> MasterMob -> SourceMob (WAVE / AIFC with
//               EssenceData, or a NetworkLocator to the file; linked originals: PCMDescriptor + NetworkLocator)
//       markers EventMobSlot of DescriptiveMarkers (frames)
//   Dictionary: the data, operation, parameter, interpolation and container definitions the composition refers to.

#include "AafFile.h"
#include "AudioPost.h"
#include "TextUtil.h"

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <system_error>

#ifndef WABCUT_VERSION
#define WABCUT_VERSION "0.0.0"
#endif

namespace wabcut::interchange {

std::string_view toString(AudioPostFiles files) { return files == AudioPostFiles::Embed ? "embed" : "separate"; }
std::string_view toString(AudioPostFormat format) { return format == AudioPostFormat::BroadcastWave ? "wav" : "aiff"; }
std::string_view toString(AudioPostRender render) {
    return render == AudioPostRender::TrimAudioFiles ? "trim" : "complete";
}
std::optional<AudioPostFiles> audioPostFilesFromString(std::string_view text) {
    if (text == "embed" || text == "encapsulate") {
        return AudioPostFiles::Embed;
    }
    if (text == "separate") {
        return AudioPostFiles::Separate;
    }
    return std::nullopt;
}
std::optional<AudioPostFormat> audioPostFormatFromString(std::string_view text) {
    if (text == "wav" || text == "bwf" || text == "broadcastWave") {
        return AudioPostFormat::BroadcastWave;
    }
    if (text == "aiff" || text == "aif") {
        return AudioPostFormat::Aiff;
    }
    return std::nullopt;
}
std::optional<AudioPostRender> audioPostRenderFromString(std::string_view text) {
    if (text == "trim") {
        return AudioPostRender::TrimAudioFiles;
    }
    if (text == "complete" || text == "copy") {
        return AudioPostRender::CopyCompleteAudioFiles;
    }
    return std::nullopt;
}

namespace detail {

std::string productVersion(const std::string& given) { return given.empty() ? std::string(WABCUT_VERSION) : given; }

std::expected<void, Error> validateAudioPostAudio(const AudioPostAudio& audio) {
    if (audio.sampleRate != 48000 && audio.sampleRate != 96000) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "sample rate must be 48000 or 96000"});
    }
    if (audio.bitsPerSample != 16 && audio.bitsPerSample != 24) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "bits per sample must be 16 or 24"});
    }
    if (audio.handleFrames < 0 || audio.handleFrames > 1000) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "handle frames must be 0 to 1000"});
    }
    return {};
}

aaf::TimeStamp timeStampOf(std::optional<std::chrono::system_clock::time_point> when) {
    const auto tp = when.value_or(std::chrono::system_clock::now());
    const auto day = std::chrono::floor<std::chrono::days>(tp);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::milliseconds>(tp - day)};
    aaf::TimeStamp ts;
    ts.year = static_cast<std::int16_t>(static_cast<int>(ymd.year()));
    ts.month = static_cast<std::uint8_t>(static_cast<unsigned>(ymd.month()));
    ts.day = static_cast<std::uint8_t>(static_cast<unsigned>(ymd.day()));
    ts.hour = static_cast<std::uint8_t>(hms.hours().count());
    ts.minute = static_cast<std::uint8_t>(hms.minutes().count());
    ts.second = static_cast<std::uint8_t>(hms.seconds().count());
    ts.fraction = static_cast<std::uint8_t>(hms.subseconds().count() / 10);
    return ts;
}

namespace {

using aaf::Auid;
using aaf::MobId;
using aaf::Object;

constexpr Auid kDataDefPicture{0x01030202, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataDefSound{0x01030202, 0x0200, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataDefTimecode{0x01030201, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataDefDescriptive{0x01030201, 0x1000, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kOpMonoAudioGain{0x9d2ea894, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kOpMonoAudioPan{0x9d2ea893, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kOpMonoAudioDissolve{0x0c3bea41, 0xfc05, 0x11d2, {0x8a, 0x29, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamLevel{0xe4962320, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamAmplitude{0xe4962321, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamPan{0xe4962322, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kInterpolationLinear{0x5b6c85a4, 0x0ede, 0x11d3, {0x80, 0xa9, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kInterpolationPower{0xc09153f7, 0xbd18, 0x4e5a, {0xad, 0x09, 0xcb, 0xdd, 0x65, 0x4f, 0xa0, 0x01}};
constexpr Auid kContainerAaf{0x4313b571, 0xd8ba, 0x11d2, {0x80, 0x9b, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kContainerExternal{0x4313b572, 0xd8ba, 0x11d2, {0x80, 0x9b, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
/// The Rational type, for parameter definitions.
constexpr Auid kTypeRational{0x03010100, 0x0000, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x04, 0x01, 0x01}};

std::string rationalBytes(std::int32_t num, std::int32_t den) {
    std::string out;
    for (const std::uint32_t v : {static_cast<std::uint32_t>(num), static_cast<std::uint32_t>(den)}) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

MobId mobIdOf(const std::array<std::uint8_t, 16>& material, std::string_view role) {
    std::string seed(reinterpret_cast<const char*>(material.data()), material.size());
    seed += role;
    return MobId::fromMaterial(Auid::fromBytes(audiopost::stableId(seed)));
}

/// A point time as a fraction of `length` (rational, denominator <= 2^30).
std::pair<std::int32_t, std::int32_t> fraction(std::int64_t offset, std::int64_t length) {
    if (length <= 0) {
        return {0, 1};
    }
    std::int64_t num = std::clamp<std::int64_t>(offset, 0, length);
    std::int64_t den = length;
    while (den > (1 << 30)) {
        num /= 2;
        den /= 2;
    }
    const std::int64_t g = std::gcd(num, den);
    return {static_cast<std::int32_t>(num / std::max<std::int64_t>(g, 1)), static_cast<std::int32_t>(den / std::max<std::int64_t>(g, 1))};
}

struct Writer {
    aaf::File file;
    const audiopost::Plan& plan;
    const AafExportOptions& options;
    aaf::TimeStamp now;
    bool embed = false;
    std::vector<MobId> sourceMasters;      // per plan source
    std::vector<MobId> videoMasters;       // per video source
    std::uint32_t slotId = 0;

    Writer(const audiopost::Plan& p, const AafExportOptions& o) : plan(p), options(o) {}

    void definitions() {
        Object& dict = file.dictionary();
        auto def = [&](std::string_view property, std::string_view cls, const Auid& id, std::string_view name,
                       std::string_view description) -> Object& {
            Object& d = dict.append(property, cls);
            d.setAuid("Identification", id).setString("Name", name);
            if (!description.empty()) {
                d.setString("Description", description);
            }
            return d;
        };
        def("DataDefinitions", "DataDefinition", kDataDefPicture, "Picture", "Picture data");
        def("DataDefinitions", "DataDefinition", kDataDefSound, "Sound", "Sound data");
        def("DataDefinitions", "DataDefinition", kDataDefTimecode, "Timecode", "Timecode data");
        def("DataDefinitions", "DataDefinition", kDataDefDescriptive, "DescriptiveMetadata", "Descriptive metadata");
        def("ParameterDefinitions", "ParameterDefinition", kParamAmplitude, "Amplitude", "")
            .setWeakRef("Type", kTypeRational);
        def("ParameterDefinitions", "ParameterDefinition", kParamPan, "Pan", "").setWeakRef("Type", kTypeRational);
        def("ParameterDefinitions", "ParameterDefinition", kParamLevel, "Level", "").setWeakRef("Type", kTypeRational);
        auto op = [&](const Auid& id, std::string_view name, int inputs, const Auid& parameter) {
            def("OperationDefinitions", "OperationDefinition", id, name, "")
                .setWeakRef("DataDefinition", kDataDefSound)
                .setBool("IsTimeWarp", false)
                .setInt("NumberInputs", inputs)
                .setWeakRefs("ParametersDefined", {parameter});
        };
        op(kOpMonoAudioGain, "Mono Audio Gain", 1, kParamAmplitude);
        op(kOpMonoAudioPan, "Mono Audio Pan", 1, kParamPan);
        op(kOpMonoAudioDissolve, "Mono Audio Dissolve", 2, kParamLevel);
        def("InterpolationDefinitions", "InterpolationDefinition", kInterpolationLinear, "Linear", "");
        def("InterpolationDefinitions", "InterpolationDefinition", kInterpolationPower, "Power", "");
        def("ContainerDefinitions", "ContainerDefinition", kContainerAaf, "AAF", "Essence in the AAF file");
        def("ContainerDefinitions", "ContainerDefinition", kContainerExternal, "External", "Essence in a separate file");
    }

    void header() {
        Object& h = file.header();
        h.setInt("ByteOrder", 0x4949)
            .setTimeStamp("LastModified", now)
            .setRaw("Version", std::string("\x01\x01", 2))
            .setInt("ObjectModelVersion", 1)
            .setAuid("OperationalPattern", aaf::kOpEditProtocol);
        Object& id = h.append("IdentificationList", "Identification");
        std::string version;
        const std::string versionString = productVersion(options.productVersion);
        int parts[3] = {0, 0, 0};
        std::string_view rest = versionString;
        for (int& p : parts) {
            const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), p);
            if (ec != std::errc{}) {
                break;
            }
            rest = rest.substr(static_cast<std::size_t>(ptr - rest.data()));
            if (!rest.empty() && rest.front() == '.') {
                rest.remove_prefix(1);
            }
        }
        for (const int p : parts) {
            const auto v = static_cast<std::uint16_t>(std::clamp(p, 0, 65535));
            version.push_back(static_cast<char>(v & 0xFF));
            version.push_back(static_cast<char>(v >> 8));
        }
        version.append(2, '\0'); // patch level
        version.push_back('\x01'); // VersionReleased
        id.setString("CompanyName", "Wabcut")
            .setString("ProductName", "Wabcut")
            .setRaw("ProductVersion", version)
            .setString("ProductVersionString", versionString)
            .setAuid("ProductID", Auid::fromBytes(audiopost::stableId("Wabcut video editor")))
            .setTimeStamp("Date", now)
            .setAuid("GenerationAUID", Auid::fromBytes(audiopost::stableId("generation\n" + plan.sequenceName +
                                                                           "\n" + versionString)))
            .setString("Platform", "Wabcut");
    }

    Object& component(Object& parent, std::string_view property, bool append, std::string_view cls, const Auid& dataDef,
                      std::int64_t length) {
        Object& c = append ? parent.append(property, cls) : parent.child(property, cls);
        c.setWeakRef("DataDefinition", dataDef).setInt("Length", length);
        return c;
    }

    void sourceClip(Object& c, const MobId& mob, std::uint32_t slot, std::int64_t start) {
        c.setInt("StartTime", start).setMobId("SourceID", mob).setInt("SourceMobSlotID", slot);
    }

    Object& timelineSlot(Object& mob, std::string_view name, std::int32_t rateNum, std::int32_t rateDen,
                         std::uint32_t physical) {
        Object& slot = mob.append("Slots", "TimelineMobSlot");
        slot.setInt("SlotID", ++slotId).setRational("EditRate", rateNum, rateDen).setInt("Origin", 0);
        if (!name.empty()) {
            slot.setString("SlotName", name);
        }
        if (physical > 0) {
            slot.setInt("PhysicalTrackNumber", physical);
        }
        return slot;
    }

    void mobTimes(Object& mob) { mob.setTimeStamp("CreationTime", now).setTimeStamp("LastModified", now); }

    /// The MobIDs the composition refers to (before it is written).
    void ids() {
        for (const audiopost::Source& s : plan.sources) {
            sourceMasters.push_back(mobIdOf(s.material, "master"));
        }
        for (const audiopost::VideoSource& v : plan.videoSources) {
            videoMasters.push_back(mobIdOf(v.material, "master"));
        }
    }

    /// The MasterMob and file SourceMob of every audio source (and its EssenceData when embedded).
    void audioSources() {
        for (const audiopost::Source& s : plan.sources) {
            const MobId fileMob = mobIdOf(s.material, "file");
            const MobId master = mobIdOf(s.material, "master");
            if (s.failed) {
                continue;
            }
            const std::int32_t rate = s.rendered ? plan.sampleRate : s.sampleRate;
            const std::int64_t fileLength =
                s.rendered ? s.length : std::max<std::int64_t>(0, s.length * s.sampleRate / std::max(plan.sampleRate, 1));
            // File source mob.
            Object& src = file.contentStorage().append("Mobs", "SourceMob");
            src.setMobId("MobID", fileMob).setString("Name", s.name);
            mobTimes(src);
            std::uint32_t saved = slotId;
            slotId = 0;
            Object& fslot = timelineSlot(src, {}, rate, 1, 1);
            Object& null = component(fslot, "Segment", false, "SourceClip", kDataDefSound, fileLength);
            sourceClip(null, MobId{}, 0, 0);
            const char* descriptorClass = !s.rendered ? "PCMDescriptor"
                                          : options.audio.format == AudioPostFormat::BroadcastWave ? "WAVEDescriptor"
                                                                                                   : "AIFCDescriptor";
            Object& d = src.child("EssenceDescription", descriptorClass);
            d.setRational("SampleRate", rate, 1).setInt("Length", fileLength);
            if (s.rendered) {
                d.setRaw("Summary", s.header);
                d.setWeakRef("ContainerFormat", embed ? kContainerAaf : kContainerExternal);
                if (!embed) {
                    d.append("Locator", "NetworkLocator")
                        .setString("URLString", pathToFileUrl(pathToUtf8(std::filesystem::absolute(s.file)), FileUrlStyle::Triple));
                }
            } else {
                const auto channels = static_cast<std::int64_t>(s.channels);
                const auto blockAlign = channels * s.bits / 8;
                d.setRational("AudioSamplingRate", s.sampleRate, 1)
                    .setInt("Channels", channels)
                    .setInt("QuantizationBits", s.bits)
                    .setInt("BlockAlign", blockAlign)
                    .setInt("AverageBPS", blockAlign * s.sampleRate)
                    .setWeakRef("ContainerFormat", kContainerExternal);
                d.append("Locator", "NetworkLocator").setString("URLString", pathToFileUrl(s.originalPath, FileUrlStyle::Triple));
            }
            if (s.rendered && embed) {
                Object& ed = file.contentStorage().append("EssenceData", "EssenceData");
                ed.setMobId("MobID", fileMob)
                    .setStream("Data", cfb::StreamSource::ofFile(s.file, 0, s.fileBytes));
            }
            // Master mob.
            Object& mm = file.contentStorage().append("Mobs", "MasterMob");
            mm.setMobId("MobID", master).setString("Name", s.name);
            mobTimes(mm);
            slotId = 0;
            Object& mslot = timelineSlot(mm, {}, plan.sampleRate, 1, 1);
            Object& clip = component(mslot, "Segment", false, "SourceClip", kDataDefSound, s.length);
            sourceClip(clip, fileMob, 1, 0);
            slotId = saved;
        }
    }

    void videoSources() {
        for (const audiopost::VideoSource& v : plan.videoSources) {
            const MobId fileMob = mobIdOf(v.material, "file");
            const MobId master = mobIdOf(v.material, "master");
            Object& src = file.contentStorage().append("Mobs", "SourceMob");
            src.setMobId("MobID", fileMob).setString("Name", v.name);
            mobTimes(src);
            const std::uint32_t saved = slotId;
            slotId = 0;
            Object& fslot = timelineSlot(src, {}, plan.frameRate.num, plan.frameRate.den, 1);
            sourceClip(component(fslot, "Segment", false, "SourceClip", kDataDefPicture, v.lengthFrames), MobId{}, 0, 0);
            Object& d = src.child("EssenceDescription", "CDCIDescriptor");
            const std::int32_t w = std::max(v.width, 1);
            const std::int32_t h = std::max(v.height, 1);
            const std::int32_t g = std::gcd(w, h);
            std::string lineMap;
            for (const std::uint32_t line : {0u, 0u}) {
                for (int i = 0; i < 4; ++i) {
                    lineMap.push_back(static_cast<char>((line >> (8 * i)) & 0xFF));
                }
            }
            d.setRational("SampleRate", v.frameRate.num, v.frameRate.den)
                .setInt("Length", v.lengthFrames)
                .setInt("StoredWidth", w)
                .setInt("StoredHeight", h)
                .setInt("FrameLayout", 0) // FullFrame
                .setRaw("VideoLineMap", lineMap)
                .setRational("ImageAspectRatio", w / g, h / g)
                .setInt("ComponentWidth", 8)
                .setInt("HorizontalSubsampling", 2)
                .setWeakRef("ContainerFormat", kContainerExternal);
            d.append("Locator", "NetworkLocator").setString("URLString", pathToFileUrl(v.path, FileUrlStyle::Triple));
            Object& mm = file.contentStorage().append("Mobs", "MasterMob");
            mm.setMobId("MobID", master).setString("Name", v.name);
            mobTimes(mm);
            slotId = 0;
            Object& mslot = timelineSlot(mm, {}, plan.frameRate.num, plan.frameRate.den, 1);
            sourceClip(component(mslot, "Segment", false, "SourceClip", kDataDefPicture, v.lengthFrames), fileMob, 1, 0);
            slotId = saved;
        }
    }

    /// A gain or pan as the Parameters of an OperationGroup of `length`.
    void parameter(Object& group, const Auid& definition, bool constant, double constantValue,
                   const std::vector<std::pair<std::int64_t, double>>& points, std::int64_t length, bool decibels,
                   std::int64_t offset) {
        auto valueBytes = [&](double v) {
            if (decibels) {
                const auto [n, d] = audiopost::amplitudeRational(v);
                return rationalBytes(n, d);
            }
            constexpr std::int32_t kDen = 1 << 20;
            return rationalBytes(static_cast<std::int32_t>(std::llround(std::clamp(v, 0.0, 1.0) * kDen)), kDen);
        };
        if (constant) {
            group.append("Parameters", "ConstantValue")
                .setAuid("Definition", definition)
                .setIndirect("Value", "Rational", valueBytes(constantValue));
            return;
        }
        Object& varying = group.append("Parameters", "VaryingValue");
        varying.setAuid("Definition", definition).setWeakRef("Interpolation", kInterpolationLinear);
        std::vector<std::pair<std::int64_t, double>> pts = points;
        // Points outside the group are clamped to its ends; at least the two ends are given.
        if (pts.empty() || pts.front().first + offset > 0) {
            pts.insert(pts.begin(), {-offset, pts.empty() ? constantValue : pts.front().second});
        }
        if (pts.back().first + offset < length) {
            pts.emplace_back(length - offset, pts.back().second);
        }
        std::int64_t last = -1;
        for (const auto& [at, value] : pts) {
            const std::int64_t t = std::clamp<std::int64_t>(at + offset, 0, length);
            if (t == last) {
                continue;
            }
            last = t;
            const auto [num, den] = fraction(t, length);
            varying.append("PointList", "ControlPoint").setRational("Time", num, den).setIndirect("Value", "Rational", valueBytes(value));
        }
    }

    void transition(Object& sequence, const audiopost::Fade& f) {
        const std::int64_t length = f.before + f.after;
        Object& t = component(sequence, "Components", true, "Transition", kDataDefSound, length);
        t.setInt("CutPoint", f.kind == audiopost::Fade::Kind::Out ? length : f.before);
        Object& og = component(t, "OperationGroup", false, "OperationGroup", kDataDefSound, length);
        og.setWeakRef("Operation", kOpMonoAudioDissolve);
        Object& level = og.append("Parameters", "VaryingValue");
        level.setAuid("Definition", kParamLevel).setWeakRef("Interpolation", f.constantPower ? kInterpolationPower : kInterpolationLinear);
        level.append("PointList", "ControlPoint").setRational("Time", 0, 1).setIndirect("Value", "Rational", rationalBytes(0, 1));
        level.append("PointList", "ControlPoint").setRational("Time", 1, 1).setIndirect("Value", "Rational", rationalBytes(1, 1));
    }

    void audioTrack(Object& composition, const audiopost::PlanTrack& track, std::uint32_t physical) {
        Object& slot = timelineSlot(composition, track.name, plan.sampleRate, 1, physical);
        Object* holder = &slot;
        std::string_view holderProperty = "Segment";
        const std::int64_t length = plan.length;
        if (!track.pan.isCentre()) {
            Object& pan = component(*holder, holderProperty, false, "OperationGroup", kDataDefSound, length);
            pan.setWeakRef("Operation", kOpMonoAudioPan);
            parameter(pan, kParamPan, track.pan.points.empty(), track.pan.constant, track.pan.points, length, false, 0);
            holder = &pan;
            holderProperty = "InputSegments";
        }
        if (!track.volume.isUnity()) {
            Object& gain = holderProperty == "Segment" ? component(*holder, "Segment", false, "OperationGroup", kDataDefSound, length)
                                                       : component(*holder, "InputSegments", true, "OperationGroup", kDataDefSound, length);
            gain.setWeakRef("Operation", kOpMonoAudioGain);
            parameter(gain, kParamAmplitude, track.volume.points.empty(), track.volume.constantDb, track.volume.points,
                      length, true, 0);
            holder = &gain;
            holderProperty = "InputSegments";
        }
        Object& seq = holderProperty == "Segment" ? component(*holder, "Segment", false, "Sequence", kDataDefSound, length)
                                                  : component(*holder, "InputSegments", true, "Sequence", kDataDefSound, length);
        seq.touch("Components");
        writeComponents(seq, track);
    }

    void writeComponents(Object& seq, const audiopost::PlanTrack& track) {
        using Kind = audiopost::Fade::Kind;
        std::int64_t cursor = 0;
        bool afterFadeOut = false;
        auto fadeAt = [&](std::int64_t cut, Kind kind) -> const audiopost::Fade* {
            for (const audiopost::Fade& f : track.fades) {
                if (f.cut == cut && f.kind == kind) {
                    return &f;
                }
            }
            return nullptr;
        };
        auto filler = [&](std::int64_t length) {
            if (length > 0) {
                component(seq, "Components", true, "Filler", kDataDefSound, length);
            }
        };
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            const audiopost::ClipRef& c = track.clips[i];
            const audiopost::Fade* cross = i > 0 && track.clips[i - 1].start + track.clips[i - 1].length == c.start
                                               ? fadeAt(c.start, Kind::Cross)
                                               : nullptr;
            const audiopost::Fade* fadeIn = cross ? nullptr : fadeAt(c.start, Kind::In);
            const audiopost::Fade* crossOut = i + 1 < track.clips.size() && c.start + c.length == track.clips[i + 1].start
                                                  ? fadeAt(c.start + c.length, Kind::Cross)
                                                  : nullptr;
            const audiopost::Fade* fadeOut = crossOut ? nullptr : fadeAt(c.start + c.length, Kind::Out);
            if (cross) {
                transition(seq, *cross);
            } else if (fadeIn) {
                filler(c.start + fadeIn->after - cursor);
                transition(seq, *fadeIn);
            } else if (c.start > cursor || afterFadeOut) {
                filler(c.start - cursor);
            }
            const std::int64_t head = cross ? cross->before : 0;
            const std::int64_t tail = crossOut ? crossOut->after : 0;
            const std::int64_t segLength = c.length + head + tail;
            Object* holder = &seq;
            if (!c.gain.isUnity()) {
                Object& og = component(seq, "Components", true, "OperationGroup", kDataDefSound, segLength);
                og.setWeakRef("Operation", kOpMonoAudioGain);
                parameter(og, kParamAmplitude, c.gain.points.empty(), c.gain.constantDb, c.gain.points, segLength, true, head);
                holder = &og;
            }
            Object& sc = component(*holder, holder == &seq ? "Components" : "InputSegments", true, "SourceClip", kDataDefSound,
                                   segLength);
            sourceClip(sc, sourceMasters[c.source], 1, c.sourceStart - head);
            cursor = c.start + c.length + tail;
            afterFadeOut = false;
            if (fadeOut) {
                transition(seq, *fadeOut);
                cursor -= fadeOut->before;
                afterFadeOut = true;
            }
        }
        if (afterFadeOut) { // the fade out needs filler to fade into
            const std::int64_t end = std::max(plan.length, cursor + 1);
            filler(end - cursor);
            cursor = end;
        }
        filler(plan.length - cursor);
    }

    void videoTrack(Object& composition, const audiopost::VideoTrack& track, std::uint32_t physical) {
        Object& slot = timelineSlot(composition, track.name, plan.frameRate.num, plan.frameRate.den, physical);
        Object& seq = component(slot, "Segment", false, "Sequence", kDataDefPicture, plan.lengthFrames);
        std::int64_t cursor = 0;
        for (const audiopost::VideoClip& c : track.clips) {
            if (c.start < cursor) {
                continue;
            }
            if (c.start > cursor) {
                component(seq, "Components", true, "Filler", kDataDefPicture, c.start - cursor);
            }
            sourceClip(component(seq, "Components", true, "SourceClip", kDataDefPicture, c.length), videoMasters[c.source], 1,
                       c.sourceStart);
            cursor = c.start + c.length;
        }
        if (plan.lengthFrames > cursor) {
            component(seq, "Components", true, "Filler", kDataDefPicture, plan.lengthFrames - cursor);
        }
    }

    void composition(const MobId& id) {
        Object& mob = file.contentStorage().append("Mobs", "CompositionMob");
        mob.setMobId("MobID", id).setString("Name", plan.sequenceName).setAuid("UsageCode", aaf::kUsageTopLevel);
        mobTimes(mob);
        slotId = 0;
        // Timecode.
        Object& tc = timelineSlot(mob, "TC1", plan.frameRate.num, plan.frameRate.den, 1);
        const auto fps = static_cast<std::int64_t>(std::llround(plan.frameRate.toDouble()));
        component(tc, "Segment", false, "Timecode", kDataDefTimecode, plan.lengthFrames)
            .setInt("Start", plan.startTimecodeFrames)
            .setInt("FPS", std::clamp<std::int64_t>(fps, 1, 65535))
            .setInt("Drop", plan.dropFrame ? 1 : 0);
        std::uint32_t physical = 0;
        for (const audiopost::VideoTrack& v : plan.videoTracks) {
            videoTrack(mob, v, ++physical);
        }
        physical = 0;
        for (const audiopost::PlanTrack& t : plan.tracks) {
            audioTrack(mob, t, ++physical);
        }
        if (!plan.markers.empty()) {
            Object& slot = mob.append("Slots", "EventMobSlot");
            slot.setInt("SlotID", ++slotId)
                .setString("SlotName", "Markers")
                .setRational("EditRate", plan.frameRate.num, plan.frameRate.den);
            Object& seq = slot.child("Segment", "Sequence");
            seq.setWeakRef("DataDefinition", kDataDefDescriptive);
            for (const audiopost::PlanMarker& m : plan.markers) {
                Object& marker = seq.append("Components", "DescriptiveMarker");
                marker.setWeakRef("DataDefinition", kDataDefDescriptive).setInt("Position", m.frame);
                if (m.lengthFrames > 0) {
                    marker.setInt("Length", m.lengthFrames);
                }
                std::string text = m.name;
                if (!m.comment.empty()) {
                    text += text.empty() ? m.comment : ": " + m.comment;
                }
                marker.setString("Comment", text);
            }
        }
    }
};

} // namespace

} // namespace detail

std::expected<AudioPostResult, Error> exportAafToFile(const ProjectState& project, SequenceId sequence,
                                                      const std::filesystem::path& file, const AafExportOptions& options,
                                                      const AudioPostHost& host) {
    using namespace detail;
    if (auto valid = validateAudioPostAudio(options.audio); !valid) {
        return std::unexpected(valid.error());
    }
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(sequence.value) + " does not exist"});
    }
    AudioPostResult result;
    result.file = file;
    const std::string fileLabel = pathToUtf8(file.filename());
    SourceLocation seqWhere;
    seqWhere.file = fileLabel;
    seqWhere.element = "'" + seq->name + "'";

    audiopost::PlanOptions po;
    po.breakout = options.breakoutToMono;
    po.clipEffects = options.renderAudioClipEffects;
    po.cleanCopies = options.includeClipCopiesWithoutEffects;
    po.video = !(options.mixdownVideo && host.video);
    po.trackAutomation = true;
    po.audio = options.audio;
    po.projectFile = options.projectFile;
    if (po.breakout && !host.audio) {
        po.breakout = false;
        result.report.add(ReportOutcome::Approximated, ReportCategory::Media,
                          "Breakout to Mono needs Wabcut's audio engine (the app or wabcut-cli): the clips link to their "
                          "original files instead",
                          seqWhere, seq->name);
    }
    if (options.mixdownVideo && !host.video) {
        result.report.add(ReportOutcome::Approximated, ReportCategory::Media,
                          "Mixdown Video needs Wabcut's renderer (the app or wabcut-cli): the video clips link to their "
                          "original files instead",
                          seqWhere, seq->name);
    }
    auto plan = audiopost::makePlan(project, sequence, po, result.report, fileLabel);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (host.dryRun) {
        result.tracks = plan->tracks.size();
        for (const audiopost::PlanTrack& t : plan->tracks) {
            result.clips += t.clips.size();
        }
        return result;
    }
    auto progress = [&](double from, double to) {
        return [&host, from, to](double p) { return !host.progress || host.progress(from + (to - from) * p); };
    };
    const double videoShare = options.mixdownVideo && host.video ? 0.4 : 0.0;

    // Mixdown video: the reference file, one clip on V1.
    if (options.mixdownVideo && host.video) {
        auto video = host.video->render(project, sequence, file, progress(0.0, videoShare));
        if (!video) {
            return std::unexpected(video.error());
        }
        audiopost::VideoSource vs;
        vs.name = pathToUtf8(video->filename());
        vs.path = pathToUtf8(std::filesystem::absolute(*video));
        vs.material = audiopost::stableId("mixdown\n" + vs.path);
        vs.width = seq->width;
        vs.height = seq->height;
        vs.frameRate = seq->frameRate;
        vs.lengthFrames = plan->lengthFrames;
        plan->videoSources = {vs};
        audiopost::VideoTrack vt;
        vt.name = "Mixdown";
        vt.clips.push_back({vs.name, 0, plan->lengthFrames, 0, 0, seqWhere.element});
        plan->videoTracks = {vt};
        result.referenceVideo = *video;
    }

    // The new audio files.
    const bool embed = options.audio.files == AudioPostFiles::Embed;
    std::filesystem::path tempFolder;
    if (embed) {
        tempFolder = file;
        tempFolder += ".wabcut-partial-audio";
    }
    FolderGuard tempGuard(tempFolder);
    if (po.breakout && !plan->groups.empty()) {
        audiopost::RenderTarget target;
        target.kind = options.audio.format == AudioPostFormat::BroadcastWave ? audiopost::FileKind::Wave
                      : embed                                                ? audiopost::FileKind::Aifc
                                                                             : audiopost::FileKind::Aiff;
        target.separate = !embed;
        target.folder = embed ? tempFolder : file.parent_path() / pathFromUtf8(pathToUtf8(file.stem()) + " Audio");
        std::error_code ec;
        std::filesystem::create_directories(target.folder, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(target.folder)});
        }
        const aaf::TimeStamp ts = timeStampOf(options.timestamp);
        char date[16];
        char time[16];
        std::snprintf(date, sizeof date, "%04d-%02d-%02d", ts.year, ts.month, ts.day);
        std::snprintf(time, sizeof time, "%02d:%02d:%02d", ts.hour, ts.minute, ts.second);
        target.bextTemplate.originationDate = date;
        target.bextTemplate.originationTime = time;
        target.bextTemplate.originatorReference = "Wabcut " + productVersion(options.productVersion);
        target.timecodeOffsetSamples = ticksToSamples(seq->startTimecode, Rate{options.audio.sampleRate, 1});
        target.umidOf = [](const audiopost::Source& s) { return mobIdOf(s.material, "file").bytes; };
        auto rendered = audiopost::renderPlan(project, *plan, *host.audio, target, progress(videoShare, 0.9),
                                              result.report, fileLabel);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        audiopost::dropFailedClips(*plan);
        if (!embed) {
            for (const audiopost::Source& s : plan->sources) {
                if (s.rendered && !s.failed) {
                    result.audioFiles.push_back(s.file);
                }
            }
        }
    }

    // The AAF.
    Writer w(*plan, options);
    w.embed = embed;
    w.now = timeStampOf(options.timestamp);
    w.definitions();
    const MobId compositionId = MobId::fromMaterial(aaf::Auid::fromBytes(audiopost::stableId(
        "composition\n" + std::to_string(sequence.value) + "\n" + seq->name + "\n" + std::to_string(w.now.year) + "-" +
        std::to_string(w.now.month) + "-" + std::to_string(w.now.day) + " " + std::to_string(w.now.hour) + ":" +
        std::to_string(w.now.minute) + ":" + std::to_string(w.now.second))));
    w.header();
    w.ids();
    w.composition(compositionId);
    w.audioSources();
    w.videoSources();
    if (host.progress && !host.progress(0.92)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
    }
    if (auto written = w.file.write(file); !written) {
        return std::unexpected(written.error());
    }
    std::error_code ec;
    result.bytes = std::filesystem::file_size(file, ec);
    result.tracks = plan->tracks.size();
    for (const audiopost::PlanTrack& t : plan->tracks) {
        result.clips += t.clips.size();
    }
    if (host.progress) {
        host.progress(1.0);
    }
    return result;
}

} // namespace wabcut::interchange
