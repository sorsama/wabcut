// OMF 2.0 export for audio post (AudioPostExport.h): the plan (AudioPost.h) written as an OMF Interchange 2.0 file in
// a Bento container (Bento.h), as Premiere Pro's File > Export > OMF does: mono audio files rendered by the host,
// encapsulated in the OMF file or next to it, one composition of the sequence's audio tracks.
//
//   HEAD (Bento object 1): ByteOrder 'MM', Version 2.0, Mobs, PrimaryMobs (the composition), MediaData,
//                          DefinitionObjects, IdentificationList
//   CMOB "<OMF title>"
//     slot TC1  TCCP (start timecode, frames)
//     A1..An    [EFFE MonoAudioPan [EFFE MonoAudioGain [SEQU of FILL / SCLP (fade in/out) / EFFE MonoAudioGain(SCLP)
//               / TRAN(EFFE SimpleMonoAudioDissolve)]]] (samples) -> MMOB -> SMOB (WAVD / AIFD; IsOMFI, or locators)
//   WAVE / AIFC media data (Encapsulate): the whole audio file, keyed by the file source mob's MobID.
//
// Everything is big-endian ('MM'), as OMF files from the Macintosh tools are; values follow OMF Interchange
// Specification 2.1 (Avid, 1997), Appendices A, B and E.

#include "AudioPost.h"
#include "Bento.h"
#include "TextUtil.h"

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace wabcut::interchange {

namespace detail {

namespace {

/// The application code in every MobID's first 32 bits (omfi:UID: prefix, major, minor).
constexpr std::uint32_t kUidPrefix = 0x57424354; // 'WBCT'

std::string be16(std::uint16_t v) { return {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; }
std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>(v & 0xFF)};
}
std::string rational(std::int32_t num, std::int32_t den) {
    return be32(static_cast<std::uint32_t>(num)) + be32(static_cast<std::uint32_t>(den));
}
std::string cString(std::string_view text) {
    std::string out(text);
    out.erase(std::remove(out.begin(), out.end(), '\0'), out.end());
    out.push_back('\0');
    return out;
}

struct Uid {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::string bytes() const { return major == 0 && minor == 0 ? std::string(12, '\0') : be32(kUidPrefix) + be32(major) + be32(minor); }
};

Uid uidOf(const std::array<std::uint8_t, 16>& material, std::string_view role) {
    std::string seed(reinterpret_cast<const char*>(material.data()), material.size());
    seed += role;
    const auto id = audiopost::stableId(seed);
    Uid u;
    for (int i = 0; i < 4; ++i) {
        u.major = (u.major << 8) | id[static_cast<std::size_t>(i)];
        u.minor = (u.minor << 8) | id[static_cast<std::size_t>(4 + i)];
    }
    u.major |= 1; // never the null UID
    return u;
}

/// A point time as a fraction of `length` (rational, denominator <= 2^30).
std::pair<std::int32_t, std::int32_t> fractionOf(std::int64_t offset, std::int64_t length) {
    if (length <= 0) {
        return {0, 1};
    }
    std::int64_t num = std::clamp<std::int64_t>(offset, 0, length);
    std::int64_t den = length;
    while (den > (1 << 30)) {
        num /= 2;
        den /= 2;
    }
    const std::int64_t g = std::max<std::int64_t>(std::gcd(num, den), 1);
    return {static_cast<std::int32_t>(num / g), static_cast<std::int32_t>(den / g)};
}

/// Every type and property the writer uses, declared first so that their object IDs do not depend on the content.
constexpr std::string_view kNames[] = {
    "OMFI:OOBJ:ObjClass", "omfi:ClassID", "omfi:Int16", "omfi:Int32", "omfi:UInt16", "omfi:UInt32", "omfi:Boolean",
    "omfi:String", "omfi:UniqueName", "omfi:TimeStamp", "omfi:VersionType", "omfi:ObjRef", "omfi:ObjRefArray",
    "omfi:UID", "omfi:Rational", "omfi:Length32", "omfi:Position32", "omfi:DataValue", "omfi:ProductVersion",
    "omfi:ArgIDType", "omfi:FadeType", "omfi:InterpKind", "omfi:EditHintType",
    "OMFI:HEAD:ByteOrder", "OMFI:HEAD:LastModified", "OMFI:HEAD:Version", "OMFI:HEAD:Mobs", "OMFI:HEAD:PrimaryMobs",
    "OMFI:HEAD:MediaData", "OMFI:HEAD:DefinitionObjects", "OMFI:HEAD:IdentificationList",
    "OMFI:IDNT:CompanyName", "OMFI:IDNT:ProductName", "OMFI:IDNT:ProductVersion", "OMFI:IDNT:ProductVersionString",
    "OMFI:IDNT:ProductID", "OMFI:IDNT:Date", "OMFI:IDNT:Platform",
    "OMFI:DDEF:DataKindID", "OMFI:EDEF:EffectID", "OMFI:EDEF:EffectName", "OMFI:EDEF:EffectDescription",
    "OMFI:EDEF:Bypass", "OMFI:EDEF:IsTimeWarp",
    "OMFI:MOBJ:MobID", "OMFI:MOBJ:Name", "OMFI:MOBJ:Slots", "OMFI:MOBJ:LastModified", "OMFI:MOBJ:CreationTime",
    "OMFI:MSLT:Segment", "OMFI:MSLT:EditRate", "OMFI:MSLT:TrackDesc",
    "OMFI:TRKD:Origin", "OMFI:TRKD:TrackID", "OMFI:TRKD:TrackName", "OMFI:TRKD:PhysicalTrack",
    "OMFI:CPNT:DataKind", "OMFI:CPNT:Length", "OMFI:SEQU:Components",
    "OMFI:SCLP:SourceID", "OMFI:SCLP:SourceTrackID", "OMFI:SCLP:StartTime", "OMFI:SCLP:FadeInLength",
    "OMFI:SCLP:FadeInType", "OMFI:SCLP:FadeOutLength", "OMFI:SCLP:FadeOutType",
    "OMFI:TRAN:CutPoint", "OMFI:TRAN:Effect", "OMFI:EFFE:EffectKind", "OMFI:EFFE:EffectSlots", "OMFI:ESLT:ArgID",
    "OMFI:ESLT:ArgValue", "OMFI:CVAL:Value", "OMFI:VVAL:Interpolation", "OMFI:VVAL:PointList", "OMFI:CTLP:Time",
    "OMFI:CTLP:DataKind", "OMFI:CTLP:Value", "OMFI:CTLP:EditHint",
    "OMFI:TCCP:Start", "OMFI:TCCP:FPS", "OMFI:TCCP:Drop",
    "OMFI:SMOB:MediaDescription", "OMFI:MDES:Locator", "OMFI:MDFL:IsOMFI", "OMFI:MDFL:SampleRate", "OMFI:MDFL:Length",
    "OMFI:WAVD:Summary", "OMFI:AIFD:Summary", "OMFI:MDAT:MobID", "OMFI:WAVE:AudioData", "OMFI:AIFC:AudioData",
    "OMFI:NETL:URLString", "OMFI:DOSL:PathName",
};

struct OmfWriter {
    bento::Builder b;
    const audiopost::Plan& plan;
    const OmfExportOptions& options;
    std::string now;         ///< omfi:TimeStamp
    bool embed = true;
    bool aifc = false;
    std::uint32_t ddefSound = 0, ddefStereo = 0, ddefTimecode = 0, ddefRational = 0;
    std::uint32_t edefDissolve = 0, edefGain = 0, edefPan = 0;
    std::vector<std::uint32_t> definitions, mobs, mediaData;
    std::vector<Uid> masters; // per plan source
    ImportReport& report;
    std::string fileLabel;

    OmfWriter(const audiopost::Plan& p, const OmfExportOptions& o, ImportReport& r) : plan(p), options(o), report(r) {
        for (const std::string_view name : kNames) {
            if (name.starts_with("omfi:")) {
                b.typeId(name);
            } else {
                b.propertyId(name);
            }
        }
    }

    void set(std::uint32_t object, std::string_view property, std::string_view type, std::string bytes) {
        b.set(object, b.propertyId(property), b.typeId(type), std::move(bytes));
    }
    void ref(std::uint32_t object, std::string_view property, std::uint32_t target) {
        b.setReference(object, b.propertyId(property), b.typeId("omfi:ObjRef"), be32(target), {target});
    }
    void refs(std::uint32_t object, std::string_view property, const std::vector<std::uint32_t>& targets) {
        std::string bytes = be16(static_cast<std::uint16_t>(targets.size()));
        for (const std::uint32_t t : targets) {
            bytes += be32(t);
        }
        b.setReference(object, b.propertyId(property), b.typeId("omfi:ObjRefArray"), std::move(bytes), targets);
    }
    std::uint32_t object(std::string_view cls) {
        const std::uint32_t id = b.newObject();
        set(id, "OMFI:OOBJ:ObjClass", "omfi:ClassID", std::string(cls.substr(0, 4)));
        return id;
    }
    std::uint32_t component(std::string_view cls, std::uint32_t dataKind, std::int64_t length) {
        const std::uint32_t id = object(cls);
        ref(id, "OMFI:CPNT:DataKind", dataKind);
        set(id, "OMFI:CPNT:Length", "omfi:Length32", be32(static_cast<std::uint32_t>(length)));
        return id;
    }

    void dictionary() {
        auto ddef = [&](std::string_view kind) {
            const std::uint32_t id = object("DDEF");
            set(id, "OMFI:DDEF:DataKindID", "omfi:UniqueName", cString(kind));
            definitions.push_back(id);
            return id;
        };
        ddefSound = ddef("omfi:data:Sound");
        ddefStereo = ddef("omfi:data:StereoSound");
        ddefTimecode = ddef("omfi:data:Timecode");
        ddefRational = ddef("omfi:data:Rational");
        auto edef = [&](std::string_view effect, std::string_view name, std::string_view description) {
            const std::uint32_t id = object("EDEF");
            set(id, "OMFI:EDEF:EffectID", "omfi:UniqueName", cString(effect));
            set(id, "OMFI:EDEF:EffectName", "omfi:String", cString(name));
            set(id, "OMFI:EDEF:EffectDescription", "omfi:String", cString(description));
            set(id, "OMFI:EDEF:Bypass", "omfi:ArgIDType", be32(static_cast<std::uint32_t>(-1)));
            set(id, "OMFI:EDEF:IsTimeWarp", "omfi:Boolean", std::string(1, '\0'));
            definitions.push_back(id);
            return id;
        };
        edefDissolve = edef("omfi:effect:SimpleMonoAudioDissolve", "Simple Mono Audio Dissolve",
                            "Combines two mono audio streams by using a simple linear equation.");
        edefGain = edef("omfi:effect:MonoAudioGain", "Mono Audio Gain", "Adjusts the volume of an audio Segment.");
        edefPan = edef("omfi:effect:MonoAudioPan", "Mono Audio Pan", "Splits a mono audio Segment into a L/R stereo pair.");
    }

    std::uint32_t trackDesc(std::uint32_t trackId, std::string_view name, std::uint32_t physical) {
        const std::uint32_t id = object("TRKD");
        set(id, "OMFI:TRKD:Origin", "omfi:Position32", be32(0));
        set(id, "OMFI:TRKD:TrackID", "omfi:UInt32", be32(trackId));
        if (!name.empty()) {
            set(id, "OMFI:TRKD:TrackName", "omfi:String", cString(name));
        }
        if (physical > 0) {
            set(id, "OMFI:TRKD:PhysicalTrack", "omfi:UInt32", be32(physical));
        }
        return id;
    }
    std::uint32_t slot(std::uint32_t segment, std::int32_t rateNum, std::int32_t rateDen, std::uint32_t trackDescription) {
        const std::uint32_t id = object("MSLT");
        ref(id, "OMFI:MSLT:Segment", segment);
        set(id, "OMFI:MSLT:EditRate", "omfi:Rational", rational(rateNum, rateDen));
        ref(id, "OMFI:MSLT:TrackDesc", trackDescription);
        return id;
    }
    std::uint32_t sourceClip(std::int64_t length, const Uid& source, std::uint32_t track, std::int64_t start) {
        const std::uint32_t id = component("SCLP", ddefSound, length);
        set(id, "OMFI:SCLP:SourceID", "omfi:UID", source.bytes());
        set(id, "OMFI:SCLP:SourceTrackID", "omfi:UInt32", be32(track));
        set(id, "OMFI:SCLP:StartTime", "omfi:Position32", be32(static_cast<std::uint32_t>(start)));
        return id;
    }
    void mobCommon(std::uint32_t mob, const Uid& uid, std::string_view name, const std::vector<std::uint32_t>& slots) {
        set(mob, "OMFI:MOBJ:MobID", "omfi:UID", uid.bytes());
        set(mob, "OMFI:MOBJ:Name", "omfi:String", cString(name));
        refs(mob, "OMFI:MOBJ:Slots", slots);
        set(mob, "OMFI:MOBJ:LastModified", "omfi:TimeStamp", now);
        set(mob, "OMFI:MOBJ:CreationTime", "omfi:TimeStamp", now);
        mobs.push_back(mob);
    }

    /// File source mob, master mob and (encapsulated) media data of every rendered source.
    void sources() {
        for (const audiopost::Source& s : plan.sources) {
            masters.push_back(uidOf(s.material, "master"));
            if (s.failed) {
                continue;
            }
            const Uid fileMob = uidOf(s.material, "file");
            // Descriptor.
            const std::uint32_t d = object(aifc ? "AIFD" : "WAVD");
            if (!embed) {
                const std::string path = pathToUtf8(std::filesystem::absolute(s.file));
                const std::uint32_t net = object("NETL");
                set(net, "OMFI:NETL:URLString", "omfi:String", cString(pathToFileUrl(path, FileUrlStyle::Triple)));
                std::string dos = path;
                std::replace(dos.begin(), dos.end(), '/', '\\');
                const std::uint32_t dosl = object("DOSL");
                set(dosl, "OMFI:DOSL:PathName", "omfi:String", cString(dos));
                refs(d, "OMFI:MDES:Locator", {net, dosl});
            }
            set(d, "OMFI:MDFL:IsOMFI", "omfi:Boolean", std::string(1, embed ? '\x01' : '\0'));
            set(d, "OMFI:MDFL:SampleRate", "omfi:Rational", rational(plan.sampleRate, 1));
            set(d, "OMFI:MDFL:Length", "omfi:Length32", be32(static_cast<std::uint32_t>(s.length)));
            set(d, aifc ? "OMFI:AIFD:Summary" : "OMFI:WAVD:Summary", "omfi:DataValue", s.header);
            // File source mob: one slot of the file's own (null) source.
            const std::uint32_t fileClip = sourceClip(s.length, Uid{}, 0, 0);
            const std::uint32_t fileSlot = slot(fileClip, plan.sampleRate, 1, trackDesc(1, {}, 1));
            const std::uint32_t smob = object("SMOB");
            mobCommon(smob, fileMob, s.name, {fileSlot});
            ref(smob, "OMFI:SMOB:MediaDescription", d);
            // Master mob.
            const std::uint32_t masterClip = sourceClip(s.length, fileMob, 1, 0);
            const std::uint32_t masterSlot = slot(masterClip, plan.sampleRate, 1, trackDesc(1, {}, 1));
            const std::uint32_t mmob = object("MMOB");
            mobCommon(mmob, masters.back(), s.name, {masterSlot});
            if (embed) {
                const std::uint32_t md = object(aifc ? "AIFC" : "WAVE");
                set(md, "OMFI:MDAT:MobID", "omfi:UID", fileMob.bytes());
                b.setStream(md, b.propertyId(aifc ? "OMFI:AIFC:AudioData" : "OMFI:WAVE:AudioData"), b.typeId("omfi:DataValue"),
                            cfb::StreamSource::ofFile(s.file, 0, s.fileBytes));
                mediaData.push_back(md);
            }
        }
    }

    /// A Rational value (CVAL) or a linear ramp of them (VVAL) of `length`: gain in dB as an amplitude, or pan 0..1.
    std::uint32_t control(bool constant, double value, const std::vector<std::pair<std::int64_t, double>>& points,
                          std::int64_t length, bool decibels, std::int64_t offset) {
        auto bytesOf = [&](double v) {
            if (decibels) {
                const auto [n, d] = audiopost::amplitudeRational(v);
                return rational(n, d);
            }
            constexpr std::int32_t kDen = 1 << 20;
            return rational(static_cast<std::int32_t>(std::llround(std::clamp(v, 0.0, 1.0) * kDen)), kDen);
        };
        if (constant) {
            const std::uint32_t c = component("CVAL", ddefRational, length);
            set(c, "OMFI:CVAL:Value", "omfi:DataValue", bytesOf(value));
            return c;
        }
        std::vector<std::pair<std::int64_t, double>> pts = points;
        if (pts.empty() || pts.front().first + offset > 0) {
            pts.insert(pts.begin(), {-offset, pts.empty() ? value : pts.front().second});
        }
        if (pts.back().first + offset < length) {
            pts.emplace_back(length - offset, pts.back().second);
        }
        std::vector<std::uint32_t> controlPoints;
        std::int64_t last = -1;
        for (const auto& [at, v] : pts) {
            const std::int64_t t = std::clamp<std::int64_t>(at + offset, 0, length);
            if (t == last) {
                continue;
            }
            last = t;
            const auto [num, den] = fractionOf(t, length);
            const std::uint32_t cp = object("CTLP");
            set(cp, "OMFI:CTLP:Time", "omfi:Rational", rational(num, den));
            ref(cp, "OMFI:CTLP:DataKind", ddefRational);
            set(cp, "OMFI:CTLP:Value", "omfi:DataValue", bytesOf(v));
            set(cp, "OMFI:CTLP:EditHint", "omfi:EditHintType", be16(0)); // proportional
            controlPoints.push_back(cp);
        }
        const std::uint32_t vv = component("VVAL", ddefRational, length);
        set(vv, "OMFI:VVAL:Interpolation", "omfi:InterpKind", be16(2)); // linear
        refs(vv, "OMFI:VVAL:PointList", controlPoints);
        return vv;
    }

    std::uint32_t effectSlot(std::int32_t argId, std::uint32_t value) {
        const std::uint32_t id = object("ESLT");
        set(id, "OMFI:ESLT:ArgID", "omfi:ArgIDType", be32(static_cast<std::uint32_t>(argId)));
        ref(id, "OMFI:ESLT:ArgValue", value);
        return id;
    }
    /// EFFE of `effect` over `input` (ArgID -1) with its level / amplitude / pan control (ArgID 1).
    std::uint32_t effect(std::uint32_t effectDef, std::uint32_t dataKind, std::int64_t length, std::uint32_t input,
                         std::uint32_t controlValue) {
        const std::uint32_t inputSlot = effectSlot(-1, input);
        const std::uint32_t controlSlot = effectSlot(1, controlValue);
        const std::uint32_t id = component("EFFE", dataKind, length);
        ref(id, "OMFI:EFFE:EffectKind", effectDef);
        refs(id, "OMFI:EFFE:EffectSlots", {inputSlot, controlSlot});
        return id;
    }

    std::uint32_t sequence(const audiopost::PlanTrack& track) {
        using Kind = audiopost::Fade::Kind;
        std::vector<std::uint32_t> components;
        std::int64_t cursor = 0;
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
                components.push_back(component("FILL", ddefSound, length));
            }
        };
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            const audiopost::ClipRef& c = track.clips[i];
            const audiopost::Fade* cross = i > 0 && track.clips[i - 1].start + track.clips[i - 1].length == c.start
                                               ? fadeAt(c.start, Kind::Cross)
                                               : nullptr;
            const audiopost::Fade* crossOut = i + 1 < track.clips.size() && c.start + c.length == track.clips[i + 1].start
                                                  ? fadeAt(c.start + c.length, Kind::Cross)
                                                  : nullptr;
            const audiopost::Fade* fadeIn = cross ? nullptr : fadeAt(c.start, Kind::In);
            const audiopost::Fade* fadeOut = crossOut ? nullptr : fadeAt(c.start + c.length, Kind::Out);
            if (cross) {
                // Level (ArgID -3): B over A, a linear ramp from 0 to 1, the dissolve's default made explicit.
                const std::int64_t length = cross->before + cross->after;
                const std::uint32_t level = effectSlot(-3, control(false, 0.0, {{0, 0.0}, {length, 1.0}}, length, false, 0));
                const std::uint32_t dissolve = component("EFFE", ddefSound, length);
                ref(dissolve, "OMFI:EFFE:EffectKind", edefDissolve);
                refs(dissolve, "OMFI:EFFE:EffectSlots", {level});
                const std::uint32_t t = component("TRAN", ddefSound, length);
                set(t, "OMFI:TRAN:CutPoint", "omfi:Position32", be32(static_cast<std::uint32_t>(cross->before)));
                ref(t, "OMFI:TRAN:Effect", dissolve);
                components.push_back(t);
                if (cross->constantPower) {
                    SourceLocation where;
                    where.file = fileLabel;
                    where.element = "'" + track.name + "'";
                    report.add(ReportOutcome::Approximated, ReportCategory::Transition,
                               "OMF dissolves have no curve: the equal power crossfade is written as a linear dissolve",
                               where, plan.sequenceName);
                }
            } else {
                filler(c.start - cursor);
            }
            const std::int64_t head = cross ? cross->before : 0;
            const std::int64_t tail = crossOut ? crossOut->after : 0;
            const std::int64_t length = c.length + head + tail;
            std::uint32_t clip = sourceClip(length, masters[c.source], 1, c.sourceStart - head);
            if (fadeIn) {
                set(clip, "OMFI:SCLP:FadeInLength", "omfi:Length32", be32(static_cast<std::uint32_t>(fadeIn->after)));
                set(clip, "OMFI:SCLP:FadeInType", "omfi:FadeType", be16(fadeIn->constantPower ? 2 : 1));
            }
            if (fadeOut) {
                set(clip, "OMFI:SCLP:FadeOutLength", "omfi:Length32", be32(static_cast<std::uint32_t>(fadeOut->before)));
                set(clip, "OMFI:SCLP:FadeOutType", "omfi:FadeType", be16(fadeOut->constantPower ? 2 : 1));
            }
            if (!c.gain.isUnity()) {
                clip = effect(edefGain, ddefSound, length, clip,
                              control(c.gain.points.empty(), c.gain.constantDb, c.gain.points, length, true, head));
            }
            components.push_back(clip);
            cursor = c.start + c.length + tail;
        }
        filler(plan.length - cursor);
        const std::uint32_t seq = component("SEQU", ddefSound, plan.length);
        refs(seq, "OMFI:SEQU:Components", components);
        return seq;
    }

    void composition(const Uid& id, std::string_view name) {
        std::vector<std::uint32_t> slots;
        std::uint32_t trackId = 0;
        // Timecode.
        const std::uint32_t tc = component("TCCP", ddefTimecode, plan.lengthFrames);
        const auto fps = static_cast<std::uint16_t>(std::clamp<long long>(std::llround(plan.frameRate.toDouble()), 1, 65535));
        set(tc, "OMFI:TCCP:Start", "omfi:Position32", be32(static_cast<std::uint32_t>(plan.startTimecodeFrames)));
        set(tc, "OMFI:TCCP:FPS", "omfi:UInt16", be16(fps));
        set(tc, "OMFI:TCCP:Drop", "omfi:Boolean", std::string(1, plan.dropFrame ? '\x01' : '\0'));
        slots.push_back(slot(tc, plan.frameRate.num, plan.frameRate.den, trackDesc(++trackId, "TC1", 1)));
        std::uint32_t physical = 0;
        for (const audiopost::PlanTrack& t : plan.tracks) {
            std::uint32_t segment = sequence(t);
            if (!t.volume.isUnity()) {
                segment = effect(edefGain, ddefSound, plan.length, segment,
                                 control(true, t.volume.constantDb, {}, plan.length, true, 0));
            }
            if (options.includePan && !t.pan.isCentre()) {
                segment = effect(edefPan, ddefStereo, plan.length, segment, control(true, t.pan.constant, {}, plan.length, false, 0));
            }
            slots.push_back(slot(segment, plan.sampleRate, 1, trackDesc(++trackId, t.name, ++physical)));
        }
        const std::uint32_t cmob = object("CMOB");
        mobCommon(cmob, id, name, slots);
    }

    void identification(std::uint32_t& idnt) {
        idnt = object("IDNT");
        const std::string versionString = productVersion(options.productVersion);
        std::string version;
        std::string_view rest = versionString;
        for (int i = 0; i < 3; ++i) {
            int part = 0;
            const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), part);
            if (ec == std::errc{}) {
                rest = rest.substr(static_cast<std::size_t>(ptr - rest.data()));
                if (!rest.empty() && rest.front() == '.') {
                    rest.remove_prefix(1);
                }
            }
            version += be16(static_cast<std::uint16_t>(std::clamp(part, 0, 65535)));
        }
        version += be16(0); // patch
        version += be16(1); // kVersionReleased
        set(idnt, "OMFI:IDNT:CompanyName", "omfi:String", cString("Wabcut"));
        set(idnt, "OMFI:IDNT:ProductName", "omfi:String", cString("Wabcut"));
        set(idnt, "OMFI:IDNT:ProductVersion", "omfi:ProductVersion", version);
        set(idnt, "OMFI:IDNT:ProductVersionString", "omfi:String", cString(versionString));
        set(idnt, "OMFI:IDNT:ProductID", "omfi:Int32", be32(0));
        set(idnt, "OMFI:IDNT:Date", "omfi:TimeStamp", now);
        set(idnt, "OMFI:IDNT:Platform", "omfi:String", cString("Wabcut"));
    }

    void header(std::uint32_t composition, std::uint32_t idnt) {
        const std::uint32_t h = bento::kTocObject;
        set(h, "OMFI:OOBJ:ObjClass", "omfi:ClassID", "HEAD");
        set(h, "OMFI:HEAD:ByteOrder", "omfi:Int16", "MM");
        set(h, "OMFI:HEAD:LastModified", "omfi:TimeStamp", now);
        set(h, "OMFI:HEAD:Version", "omfi:VersionType", std::string("\x02\x00", 2));
        refs(h, "OMFI:HEAD:Mobs", mobs);
        refs(h, "OMFI:HEAD:PrimaryMobs", {composition});
        refs(h, "OMFI:HEAD:MediaData", mediaData);
        refs(h, "OMFI:HEAD:DefinitionObjects", definitions);
        refs(h, "OMFI:HEAD:IdentificationList", {idnt});
    }
};

} // namespace

} // namespace detail

std::expected<AudioPostResult, Error> exportOmfToFile(const ProjectState& project, SequenceId sequence,
                                                      const std::filesystem::path& file, const OmfExportOptions& options,
                                                      const AudioPostHost& host) {
    using namespace detail;
    if (auto valid = validateAudioPostAudio(options.audio); !valid) {
        return std::unexpected(valid.error());
    }
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(sequence.value) + " does not exist"});
    }
    if (!host.audio && !host.dryRun) {
        return std::unexpected(Error{ErrorCode::Unsupported,
                                     "OMF export renders new audio files and needs Wabcut's audio engine (the app or "
                                     "wabcut-cli)"});
    }
    AudioPostResult result;
    result.file = file;
    const std::string fileLabel = pathToUtf8(file.filename());
    SourceLocation seqWhere;
    seqWhere.file = fileLabel;
    seqWhere.element = "'" + seq->name + "'";

    audiopost::PlanOptions po;
    po.breakout = true; // OMF audio is mono files, one OMF track per channel
    po.clipEffects = false;
    po.cleanCopies = false;
    po.video = false;
    po.trackAutomation = false;
    po.audio = options.audio;
    po.projectFile = options.projectFile;
    auto plan = audiopost::makePlan(project, sequence, po, result.report, fileLabel);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    std::size_t videoClips = 0;
    for (const auto& t : seq->videoTracks) {
        videoClips += t->clips.size();
    }
    if (videoClips > 0) {
        result.report.add(ReportOutcome::Dropped, ReportCategory::Clip,
                          "OMF export carries the audio only: " + std::to_string(videoClips) + " video clips are left out",
                          seqWhere, seq->name);
    }
    if (!plan->markers.empty()) {
        result.report.add(ReportOutcome::Dropped, ReportCategory::Marker,
                          "OMF 2.0 has no markers: " + std::to_string(plan->markers.size()) + " markers are left out", seqWhere,
                          seq->name);
        plan->markers.clear();
    }
    if (!options.includePan) {
        for (const audiopost::PlanTrack& t : plan->tracks) {
            if (!t.pan.isCentre()) {
                result.report.add(ReportOutcome::Dropped, ReportCategory::Track,
                                  "Include Pan is off: the pan of '" + t.name + "' is not written", seqWhere, seq->name);
            }
        }
    }
    constexpr std::int64_t kMax32 = std::numeric_limits<std::int32_t>::max();
    if (plan->length > kMax32 || plan->lengthFrames > kMax32 || plan->startTimecodeFrames > kMax32) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The sequence is too long for OMF (32-bit sample counts)"});
    }
    if (host.dryRun) {
        result.tracks = plan->tracks.size();
        for (const audiopost::PlanTrack& t : plan->tracks) {
            result.clips += t.clips.size();
        }
        return result;
    }

    const bool embed = options.audio.files == AudioPostFiles::Embed;
    const bool aifc = options.audio.format == AudioPostFormat::Aiff;
    std::filesystem::path tempFolder;
    if (embed) {
        tempFolder = file;
        tempFolder += ".wabcut-partial-audio";
    }
    FolderGuard tempGuard(tempFolder);
    if (!plan->groups.empty()) {
        audiopost::RenderTarget target;
        target.kind = aifc ? audiopost::FileKind::Aifc : audiopost::FileKind::Wave;
        target.writeBext = false;
        target.separate = !embed;
        target.folder = embed ? tempFolder : file.parent_path() / pathFromUtf8(pathToUtf8(file.stem()) + " Audio");
        std::error_code ec;
        std::filesystem::create_directories(target.folder, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(target.folder)});
        }
        auto progress = [&host](double p) { return !host.progress || host.progress(0.9 * p); };
        auto rendered = audiopost::renderPlan(project, *plan, *host.audio, target, progress, result.report, fileLabel);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        audiopost::dropFailedClips(*plan);
        for (const audiopost::Source& s : plan->sources) {
            if (s.length > kMax32) {
                return std::unexpected(Error{ErrorCode::Unsupported, "An audio file is too long for OMF (32-bit sample counts)"});
            }
            if (!embed && s.rendered && !s.failed) {
                result.audioFiles.push_back(s.file);
            }
        }
    }

    OmfWriter w(*plan, options, result.report);
    w.fileLabel = fileLabel;
    w.embed = embed;
    w.aifc = aifc;
    const auto when = options.timestamp.value_or(std::chrono::system_clock::now());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
    w.now = be32(static_cast<std::uint32_t>(std::clamp<long long>(seconds, 0, std::numeric_limits<std::uint32_t>::max()))) +
            std::string(1, '\x01'); // GMT
    w.dictionary();
    w.sources();
    const std::string title = options.title.empty() ? seq->name : options.title;
    const Uid compositionId = uidOf(audiopost::stableId("composition\n" + std::to_string(sequence.value) + "\n" + title +
                                                        "\n" + std::to_string(seconds)),
                                    "composition");
    w.composition(compositionId, title);
    const std::uint32_t composition = w.mobs.back();
    std::uint32_t idnt = 0;
    w.identification(idnt);
    w.header(composition, idnt);
    if (host.progress && !host.progress(0.92)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
    }
    auto written = w.b.write(file);
    if (!written) {
        return std::unexpected(written.error());
    }
    result.bytes = *written;
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
