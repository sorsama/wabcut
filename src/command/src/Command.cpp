#include <wabcut/command/Command.h>

#include <algorithm>
#include <array>
#include <utility>

namespace wabcut::command {

namespace {

constexpr std::array<std::pair<Capability, std::string_view>, 5> kCapabilities{{
    {Capability::Read, "Read"},
    {Capability::EditSequence, "EditSequence"},
    {Capability::EditProject, "EditProject"},
    {Capability::MediaAnalysis, "MediaAnalysis"},
    {Capability::Network, "Network"},
}};

constexpr std::array<std::pair<ErrorCode, std::string_view>, 14> kErrorCodes{{
    {ErrorCode::UnknownCommand, "UnknownCommand"},
    {ErrorCode::UnsupportedVersion, "UnsupportedVersion"},
    {ErrorCode::InvalidRequest, "InvalidRequest"},
    {ErrorCode::InvalidParams, "InvalidParams"},
    {ErrorCode::NotFound, "NotFound"},
    {ErrorCode::PermissionDenied, "PermissionDenied"},
    {ErrorCode::StaleRevision, "StaleRevision"},
    {ErrorCode::IdempotencyConflict, "IdempotencyConflict"},
    {ErrorCode::EditRejected, "EditRejected"},
    {ErrorCode::InvariantViolation, "InvariantViolation"},
    {ErrorCode::LockedTrack, "LockedTrack"},
    {ErrorCode::Cancelled, "Cancelled"},
    {ErrorCode::TimedOut, "TimedOut"},
    {ErrorCode::Internal, "Internal"},
}};

constexpr std::array<std::pair<PropertyRole, std::string_view>, 9> kRoles{{
    {PropertyRole::Timing, "timing"},
    {PropertyRole::Source, "source"},
    {PropertyRole::Enabled, "enabled"},
    {PropertyRole::Label, "label"},
    {PropertyRole::Link, "link"},
    {PropertyRole::Track, "track"},
    {PropertyRole::Motion, "motion"},
    {PropertyRole::Opacity, "opacity"},
    {PropertyRole::Audio, "audio"},
}};

CommandError invalidRequest(std::string message) { return {ErrorCode::InvalidRequest, std::move(message), nullptr}; }

template <class Id>
Json idList(const std::vector<Id>& ids) {
    Json out = Json::array();
    for (const Id& id : ids) {
        out.push_back(id.value);
    }
    return out;
}

/// Rejects fields outside `known`, so a misspelt "parms" fails instead of being ignored.
std::expected<void, CommandError> onlyKnownFields(const Json& json, std::initializer_list<std::string_view> known) {
    for (const auto& item : json.items()) {
        if (std::find(known.begin(), known.end(), item.key()) == known.end()) {
            return std::unexpected(CommandError{ErrorCode::InvalidRequest, "unknown field \"" + item.key() + "\"",
                                                Json{{"field", item.key()}}});
        }
    }
    return {};
}

std::expected<void, CommandError> readEnvelope(const Json& json, std::optional<std::uint64_t>& expectedRevision,
                                               std::optional<std::string>& idempotencyKey, bool& dryRun) {
    if (json.contains("expectedRevision") && !json["expectedRevision"].is_null()) {
        const Json& v = json["expectedRevision"];
        if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<std::int64_t>() < 0)) {
            return std::unexpected(invalidRequest("\"expectedRevision\" must be a non-negative integer"));
        }
        expectedRevision = v.is_number_unsigned() ? v.get<std::uint64_t>()
                                                  : static_cast<std::uint64_t>(v.get<std::int64_t>());
    }
    if (json.contains("idempotencyKey") && !json["idempotencyKey"].is_null()) {
        if (!json["idempotencyKey"].is_string() || json["idempotencyKey"].get<std::string>().empty()) {
            return std::unexpected(invalidRequest("\"idempotencyKey\" must be a non-empty string"));
        }
        idempotencyKey = json["idempotencyKey"].get<std::string>();
    }
    if (json.contains("dryRun") && !json["dryRun"].is_null()) {
        if (!json["dryRun"].is_boolean()) {
            return std::unexpected(invalidRequest("\"dryRun\" must be true or false"));
        }
        dryRun = json["dryRun"].get<bool>();
    }
    return {};
}

} // namespace

std::string_view toString(Capability capability) {
    for (const auto& [value, name] : kCapabilities) {
        if (value == capability) {
            return name;
        }
    }
    return "Read";
}

std::optional<Capability> capabilityFromString(std::string_view text) {
    for (const auto& [value, name] : kCapabilities) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::string_view toString(ErrorCode code) {
    for (const auto& [value, name] : kErrorCodes) {
        if (value == code) {
            return name;
        }
    }
    return "Internal";
}

std::expected<CommandRequest, CommandError> requestFromJson(const Json& json) {
    if (!json.is_object()) {
        return std::unexpected(invalidRequest("a request must be a JSON object"));
    }
    if (auto fields = onlyKnownFields(json, {"command", "version", "params", "expectedRevision", "idempotencyKey", "dryRun"});
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    CommandRequest r;
    if (!json.contains("command") || !json["command"].is_string() || json["command"].get<std::string>().empty()) {
        return std::unexpected(invalidRequest("a request needs a \"command\" name"));
    }
    r.command = json["command"].get<std::string>();
    if (json.contains("version") && !json["version"].is_null()) {
        if (!json["version"].is_number_integer() || json["version"].get<std::int64_t>() < 1 ||
            json["version"].get<std::int64_t>() > 1'000'000) {
            return std::unexpected(invalidRequest("\"version\" must be a positive integer"));
        }
        r.version = json["version"].get<int>();
    }
    if (json.contains("params") && !json["params"].is_null()) {
        if (!json["params"].is_object()) {
            return std::unexpected(invalidRequest("\"params\" must be a JSON object"));
        }
        r.params = json["params"];
    }
    if (auto envelope = readEnvelope(json, r.expectedRevision, r.idempotencyKey, r.dryRun); !envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    return r;
}

bool isBatchJson(const Json& json) { return json.is_object() && json.contains("commands"); }

std::expected<CommandBatch, CommandError> batchFromJson(const Json& json) {
    if (!isBatchJson(json) || !json["commands"].is_array()) {
        return std::unexpected(invalidRequest("a batch needs a \"commands\" array"));
    }
    if (auto fields = onlyKnownFields(json, {"commands", "label", "expectedRevision", "idempotencyKey", "dryRun"}); !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    CommandBatch b;
    if (json.contains("label") && !json["label"].is_null()) {
        if (!json["label"].is_string()) {
            return std::unexpected(invalidRequest("\"label\" must be a string"));
        }
        b.label = json["label"].get<std::string>();
    }
    if (auto envelope = readEnvelope(json, b.expectedRevision, b.idempotencyKey, b.dryRun); !envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    std::size_t index = 0;
    for (const Json& item : json["commands"]) {
        auto request = requestFromJson(item);
        if (!request) {
            CommandError e = std::move(request.error());
            if (!e.details.is_object()) {
                e.details = Json::object();
            }
            e.details["index"] = index;
            return std::unexpected(std::move(e));
        }
        if (request->expectedRevision || request->idempotencyKey || request->dryRun) {
            return std::unexpected(CommandError{ErrorCode::InvalidRequest,
                                                "expectedRevision, idempotencyKey and dryRun belong on the batch, "
                                                "not on its commands",
                                                Json{{"index", index}}});
        }
        b.commands.push_back(std::move(*request));
        ++index;
    }
    return b;
}

Json toJson(const CommandRequest& request) {
    Json out{{"command", request.command}, {"params", request.params}};
    if (request.version) {
        out["version"] = *request.version;
    }
    if (request.expectedRevision) {
        out["expectedRevision"] = *request.expectedRevision;
    }
    if (request.idempotencyKey) {
        out["idempotencyKey"] = *request.idempotencyKey;
    }
    if (request.dryRun) {
        out["dryRun"] = true;
    }
    return out;
}

Json toJson(const CommandBatch& batch) {
    Json commands = Json::array();
    for (const CommandRequest& r : batch.commands) {
        commands.push_back(toJson(r));
    }
    Json out{{"commands", std::move(commands)}};
    if (!batch.label.empty()) {
        out["label"] = batch.label;
    }
    if (batch.expectedRevision) {
        out["expectedRevision"] = *batch.expectedRevision;
    }
    if (batch.idempotencyKey) {
        out["idempotencyKey"] = *batch.idempotencyKey;
    }
    if (batch.dryRun) {
        out["dryRun"] = true;
    }
    return out;
}

Json toJson(const CommandError& error) {
    Json out{{"code", std::string(toString(error.code))}, {"message", error.message}};
    if (!error.details.is_null()) {
        out["details"] = error.details;
    }
    return out;
}

Json toJson(const ChangeSet& changes) {
    Json sequences = Json::array();
    for (const SequenceChange& s : changes.sequences) {
        Json tracks = Json::array();
        for (const TrackChange& t : s.tracks) {
            Json ranges = Json::array();
            for (const TimeRange& r : t.ranges) {
                ranges.push_back(Json::array({r.start.value, r.end().value}));
            }
            Json track{{"track", t.track.value}, {"ranges", std::move(ranges)}};
            if (t.headerChanged) {
                track["headerChanged"] = true;
            }
            if (t.transitionsChanged) {
                track["transitionsChanged"] = true;
            }
            if (t.added) {
                track["added"] = true;
            }
            if (t.removed) {
                track["removed"] = true;
            }
            tracks.push_back(std::move(track));
        }
        Json roles = Json::array();
        for (const auto& [role, name] : kRoles) {
            if (s.roles.has(role)) {
                roles.push_back(std::string(name));
            }
        }
        Json flags = Json::array();
        for (const auto& [set, name] :
             {std::pair{s.settingsChanged, "settings"}, std::pair{s.marksChanged, "marks"},
              std::pair{s.markersChanged, "markers"}, std::pair{s.extentChanged, "extent"},
              std::pair{s.captionsChanged, "captions"}, std::pair{s.beatGridChanged, "beatGrid"},
              std::pair{s.trackListChanged, "trackList"}}) {
            if (set) {
                flags.push_back(name);
            }
        }
        Json sequence{{"sequence", s.sequence.value},
                      {"tracks", std::move(tracks)},
                      {"addedClips", idList(s.addedClips)},
                      {"removedClips", idList(s.removedClips)},
                      {"modifiedClips", idList(s.modifiedClips)},
                      {"roles", std::move(roles)}};
        if (!flags.empty()) {
            sequence["changed"] = std::move(flags);
        }
        sequences.push_back(std::move(sequence));
    }
    Json out{{"sequences", std::move(sequences)}};
    if (!changes.addedSequences.empty()) {
        out["addedSequences"] = idList(changes.addedSequences);
    }
    if (!changes.removedSequences.empty()) {
        out["removedSequences"] = idList(changes.removedSequences);
    }
    if (!changes.addedMedia.empty()) {
        out["addedMedia"] = idList(changes.addedMedia);
    }
    if (!changes.removedMedia.empty()) {
        out["removedMedia"] = idList(changes.removedMedia);
    }
    if (!changes.modifiedMedia.empty()) {
        out["modifiedMedia"] = idList(changes.modifiedMedia);
    }
    if (changes.binsChanged) {
        out["binsChanged"] = true;
    }
    if (changes.brandKitChanged) {
        out["brandKitChanged"] = true;
    }
    if (changes.templatesChanged) {
        out["templatesChanged"] = true;
    }
    return out;
}

Json toJson(const CommandResult& result) {
    Json out{{"ok", result.ok()}, {"revision", result.revision}};
    if (result.error) {
        out["error"] = toJson(*result.error);
        return out;
    }
    // Empty parts are left out: results are read by agents, and every token counts.
    if (!result.changes.empty()) {
        out["changes"] = toJson(result.changes);
    }
    if (!result.createdIds.empty()) {
        out["createdIds"] = result.createdIds;
    }
    if (!result.warnings.empty()) {
        out["warnings"] = result.warnings;
    }
    if (!result.data.is_null()) {
        out["data"] = result.data;
    }
    if (!result.steps.empty()) {
        Json steps = Json::array();
        for (const StepResult& s : result.steps) {
            Json step{{"command", s.command}};
            if (!s.createdIds.empty()) {
                step["createdIds"] = s.createdIds;
            }
            if (!s.data.is_null()) {
                step["data"] = s.data;
            }
            if (!s.warnings.empty()) {
                step["warnings"] = s.warnings;
            }
            steps.push_back(std::move(step));
        }
        out["steps"] = std::move(steps);
    }
    out["dryRun"] = result.dryRun;
    out["committed"] = result.committed;
    if (result.preview) {
        out["preview"] = true;
    }
    if (result.replayed) {
        out["replayed"] = true;
    }
    return out;
}

} // namespace wabcut::command
