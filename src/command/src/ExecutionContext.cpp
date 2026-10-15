#include <wabcut/command/ExecutionContext.h>

#include <limits>

#include <wabcut/model/Timecode.h>

#include <cctype>

namespace wabcut::command {

void fail(ErrorCode code, std::string message, Json details) {
    throw CommandFailure{CommandError{code, std::move(message), std::move(details)}};
}

namespace {

std::string pathOf(std::string_view key) { return "/" + std::string(key); }

Json at(std::string_view path) { return Json{{"path", std::string(path)}}; }

/// A non-negative integer as an id value. JSON parsed from text stores positive numbers as unsigned, JSON
/// built in code as signed; both are accepted.
std::optional<std::uint64_t> idValue(const Json& v) {
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    if (v.is_number_integer() && v.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(v.get<std::int64_t>());
    }
    return std::nullopt;
}

} // namespace

ExecutionContext::ExecutionContext(const ProjectState& state, const Json& params, ExecutionControl* control,
                                   CommandServices* services)
    : m_state(state), m_params(params), m_control(control), m_services(services) {}

void ExecutionContext::checkpoint(std::string_view message) {
    if (m_control) {
        if (auto stop = m_control->checkpoint(message)) {
            throw CommandFailure{std::move(*stop)};
        }
    }
}

const Sequence& ExecutionContext::sequence() {
    if (m_sequence) {
        return *m_sequence;
    }
    if (has("sequence")) {
        const Json& v = m_params["sequence"];
        const auto id = idValue(v);
        const Sequence* s = id ? m_state.findSequence(SequenceId{*id}) : nullptr;
        if (!s) {
            fail(ErrorCode::NotFound, "no sequence with id " + v.dump(), at("/sequence"));
        }
        m_sequence = s;
    } else if (m_state.sequences.size() == 1) {
        m_sequence = m_state.sequences.front().get();
    } else {
        fail(ErrorCode::InvalidParams,
             m_state.sequences.empty() ? "the project has no sequence"
                                       : "the project has several sequences: pass \"sequence\"",
             at("/sequence"));
    }
    return *m_sequence;
}

bool ExecutionContext::has(std::string_view key) const {
    return m_params.is_object() && m_params.contains(key) && !m_params[std::string(key)].is_null();
}

Ticks ExecutionContext::timeValue(const Json& value, const std::string& path) {
    if (value.is_number_integer()) {
        return {value.get<std::int64_t>()};
    }
    if (value.is_string()) {
        const Sequence& s = sequence();
        const auto parsed = parseTimecode(value.get<std::string>(), s.frameRate, s.dropFrame);
        if (!parsed) {
            fail(ErrorCode::InvalidParams, parsed.error().message, at(path));
        }
        return *parsed;
    }
    fail(ErrorCode::InvalidParams, "expected ticks or a timecode", at(path));
}

Ticks ExecutionContext::time(std::string_view key) {
    if (!has(key)) {
        fail(ErrorCode::InvalidParams, "missing \"" + std::string(key) + "\"", at(pathOf(key)));
    }
    return timeValue(m_params[std::string(key)], pathOf(key));
}

std::optional<Ticks> ExecutionContext::optionalTime(std::string_view key) {
    if (!has(key)) {
        return std::nullopt;
    }
    return timeValue(m_params[std::string(key)], pathOf(key));
}

TrackId ExecutionContext::trackValue(const Json& value, const std::string& path) {
    const Sequence& s = sequence();
    if (const auto raw = idValue(value)) {
        const TrackId id{*raw};
        if (!s.findTrack(id)) {
            fail(ErrorCode::NotFound, "no track with id " + value.dump() + " in this sequence", at(path));
        }
        return id;
    }
    if (value.is_string()) {
        const std::string label = value.get<std::string>();
        if (label.size() >= 2 && (label[0] == 'V' || label[0] == 'A' || label[0] == 'v' || label[0] == 'a')) {
            std::size_t number = 0;
            bool digits = true;
            for (std::size_t i = 1; i < label.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(label[i]))) {
                    digits = false;
                    break;
                }
                number = number * 10 + static_cast<std::size_t>(label[i] - '0');
                if (number > 10'000) {
                    digits = false;
                    break;
                }
            }
            const auto& list = (label[0] == 'V' || label[0] == 'v') ? s.videoTracks : s.audioTracks;
            if (digits && number >= 1 && number <= list.size()) {
                return list[number - 1]->id;
            }
        }
        fail(ErrorCode::NotFound, "no track \"" + label + "\" in this sequence", at(path));
    }
    fail(ErrorCode::InvalidParams, "expected a track label or id", at(path));
}

TrackId ExecutionContext::track(std::string_view key) {
    if (!has(key)) {
        fail(ErrorCode::InvalidParams, "missing \"" + std::string(key) + "\"", at(pathOf(key)));
    }
    return trackValue(m_params[std::string(key)], pathOf(key));
}

std::vector<TrackId> ExecutionContext::tracks(std::string_view key) {
    std::vector<TrackId> out;
    if (!has(key)) {
        return out;
    }
    const Json& list = m_params[std::string(key)];
    for (std::size_t i = 0; i < list.size(); ++i) {
        out.push_back(trackValue(list[i], pathOf(key) + "/" + std::to_string(i)));
    }
    return out;
}

ClipId ExecutionContext::clipValue(const Json& value, const std::string& path) {
    const Sequence& s = sequence();
    if (const auto raw = idValue(value)) {
        const ClipId id{*raw};
        if (!s.findClip(id)) {
            fail(ErrorCode::NotFound, "no clip with id " + value.dump() + " in this sequence", at(path));
        }
        return id;
    }
    if (value.is_object()) {
        const TrackId trackId = trackValue(value["track"], path + "/track");
        const Ticks t = timeValue(value["time"], path + "/time");
        const Track& track = *s.findTrack(trackId);
        if (const auto index = track.clipIndexAt(t)) {
            return track.clips[*index].id;
        }
        fail(ErrorCode::NotFound, "no clip on " + value["track"].dump() + " at " + value["time"].dump(), at(path));
    }
    fail(ErrorCode::InvalidParams, "expected a clip id or {\"track\", \"time\"}", at(path));
}

ClipId ExecutionContext::clip(std::string_view key) {
    if (!has(key)) {
        fail(ErrorCode::InvalidParams, "missing \"" + std::string(key) + "\"", at(pathOf(key)));
    }
    return clipValue(m_params[std::string(key)], pathOf(key));
}

std::vector<ClipId> ExecutionContext::clips(std::string_view key) {
    std::vector<ClipId> out;
    if (!has(key)) {
        return out;
    }
    const Json& list = m_params[std::string(key)];
    for (std::size_t i = 0; i < list.size(); ++i) {
        out.push_back(clipValue(list[i], pathOf(key) + "/" + std::to_string(i)));
    }
    return out;
}

MediaId ExecutionContext::media(std::string_view key) {
    if (!has(key)) {
        fail(ErrorCode::InvalidParams, "missing \"" + std::string(key) + "\"", at(pathOf(key)));
    }
    return mediaValue(m_params[std::string(key)], pathOf(key));
}

std::vector<MediaId> ExecutionContext::mediaList(std::string_view key) {
    std::vector<MediaId> out;
    if (!has(key)) {
        return out;
    }
    const Json& list = m_params[std::string(key)];
    for (std::size_t i = 0; i < list.size(); ++i) {
        out.push_back(mediaValue(list[i], pathOf(key) + "/" + std::to_string(i)));
    }
    return out;
}

MediaId ExecutionContext::mediaValue(const Json& v, const std::string& path) {
    if (const auto raw = idValue(v)) {
        const MediaId id{*raw};
        if (!m_state.findMedia(id)) {
            fail(ErrorCode::NotFound, "no media with id " + v.dump(), at(path));
        }
        return id;
    }
    if (v.is_string()) {
        const MediaItem* match = nullptr;
        for (const MediaPtr& m : m_state.media) {
            if (m->name == v.get<std::string>()) {
                if (match) {
                    fail(ErrorCode::InvalidParams, "several media items are named " + v.dump() + ": use the id",
                         at(path));
                }
                match = m.get();
            }
        }
        if (!match) {
            fail(ErrorCode::NotFound, "no media named " + v.dump(), at(path));
        }
        return match->id;
    }
    fail(ErrorCode::InvalidParams, "expected a media id or name", at(path));
}

std::string ExecutionContext::string(std::string_view key, std::string fallback) const {
    return has(key) ? m_params[std::string(key)].get<std::string>() : std::move(fallback);
}

std::int64_t ExecutionContext::integer(std::string_view key, std::int64_t fallback) const {
    if (!has(key)) {
        return fallback;
    }
    const Json& v = m_params[std::string(key)];
    // A JSON integer above int64 (e.g. 18446744073709551615) must not wrap to a negative value or an index.
    if (v.is_number_unsigned() && v.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        fail(ErrorCode::InvalidParams, "\"" + std::string(key) + "\" is out of range", at(pathOf(key)));
    }
    return v.get<std::int64_t>();
}

std::size_t ExecutionContext::indexValue(const Json& value, const std::string& path) const {
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail(ErrorCode::InvalidParams, "an index is out of range", at(path));
        }
        return static_cast<std::size_t>(v);
    }
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) {
        fail(ErrorCode::InvalidParams, "expected an index (an integer >= 0)", at(path));
    }
    return static_cast<std::size_t>(value.get<std::int64_t>());
}

double ExecutionContext::number(std::string_view key) const {
    if (!has(key)) {
        fail(ErrorCode::InvalidParams, "missing \"" + std::string(key) + "\"", at(pathOf(key)));
    }
    return m_params[std::string(key)].get<double>();
}

bool ExecutionContext::boolean(std::string_view key, bool fallback) const {
    return has(key) ? m_params[std::string(key)].get<bool>() : fallback;
}

edit::LinkMode ExecutionContext::linkMode() const {
    return boolean("linked", true) ? edit::LinkMode::FollowLinks : edit::LinkMode::IgnoreLinks;
}

void ExecutionContext::failFromModel(const Error& error) {
    fail(error.code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::EditRejected, error.message);
}

edit::EditResult ExecutionContext::take(edit::EditOutcome outcome) {
    if (!outcome) {
        failFromModel(outcome.error());
    }
    return std::move(*outcome);
}

std::string ExecutionContext::timecode(Ticks t) {
    const Sequence& s = sequence();
    return ticksToTimecode(t, s.frameRate, s.dropFrame);
}

std::string ExecutionContext::trackLabel(TrackId track) {
    const auto pos = sequence().trackPosition(track);
    if (!pos) {
        return std::to_string(track.value);
    }
    return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
}

} // namespace wabcut::command
