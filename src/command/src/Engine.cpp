#include <wabcut/command/Engine.h>

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <utility>

namespace wabcut::command {

namespace {

/// `path` resolved (symlinks, junctions, "..") as far as it exists; empty when it cannot be.
std::filesystem::path resolved(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path out = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::path() : out.lexically_normal();
}

bool inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty() || path.root_name() != root.root_name()) {
        return false;
    }
    const std::filesystem::path rel = path.lexically_relative(root);
    return !rel.empty() && rel.is_relative() && *rel.begin() != "..";
}

/// Why the files `params` name (spec.fileParams) are not all inside `roots`, or nullopt (S5-P1-1).
std::optional<std::string> outsideRoots(const CommandSpec& spec, const Json& params,
                                        const std::vector<std::filesystem::path>& roots) {
    if (spec.fileParams.empty()) {
        return spec.name + " cannot be confined to the allowed folders";
    }
    std::vector<std::filesystem::path> resolvedRoots;
    for (const std::filesystem::path& root : roots) {
        if (std::filesystem::path r = resolved(root); !r.empty()) {
            resolvedRoots.push_back(std::move(r));
        }
    }
    for (const std::string& key : spec.fileParams) {
        const auto it = params.find(key);
        if (it == params.end() || it->is_null()) {
            continue;
        }
        std::vector<std::string> values;
        if (it->is_string()) {
            values.push_back(it->get<std::string>());
        } else if (it->is_array()) {
            for (const Json& v : *it) {
                if (!v.is_string()) {
                    return "\"" + key + "\" must name files by path";
                }
                values.push_back(v.get<std::string>());
            }
        } else {
            return "\"" + key + "\" must name a file by path";
        }
        for (const std::string& value : values) {
            const std::filesystem::path given = pathFromUtf8(value);
            if (!given.is_absolute()) {
                return "\"" + key + "\" must be an absolute path inside the allowed folders";
            }
            const std::filesystem::path real = resolved(given);
            if (std::none_of(resolvedRoots.begin(), resolvedRoots.end(),
                             [&](const std::filesystem::path& root) { return inside(real, root); })) {
                return "\"" + key + "\" is outside the folders this client may use";
            }
        }
    }
    return std::nullopt;
}

CommandError indexed(CommandError error, bool batch, std::size_t index) {
    if (batch) {
        if (!error.details.is_object()) {
            error.details = Json::object();
        }
        error.details["index"] = index;
        error.message = "command " + std::to_string(index) + ": " + error.message;
    }
    return error;
}

/// The first locked track whose clips or transitions differ between `before` and `after`. Tracks are
/// shared nodes, so unchanged tracks compare by pointer. Header changes (unlocking) are allowed.
std::optional<std::string> lockedTrackChange(const ProjectState& before, const ProjectState& after) {
    for (const SequencePtr& seq : before.sequences) {
        const Sequence* next = after.findSequence(seq->id);
        if (!next) {
            continue; // removing a whole sequence is not a timeline edit
        }
        for (const auto* list : {&seq->videoTracks, &seq->audioTracks}) {
            for (const TrackPtr& track : *list) {
                if (!track->locked) {
                    continue;
                }
                const Track* now = next->findTrack(track->id);
                if (!now) {
                    return "a locked track was deleted";
                }
                if (now != track.get() && (now->clips != track->clips || now->transitions != track->transitions)) {
                    return "the edit would change locked track " +
                           std::string(next->trackPosition(track->id)->kind == TrackKind::Video ? "V" : "A") +
                           std::to_string(next->trackPosition(track->id)->index + 1);
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

CommandEngine::CommandEngine(EditSession& session, const CommandRegistry& registry, EnginePolicy policy)
    : m_session(session), m_registry(registry), m_policy(policy) {
    // Every engine has a clipboard (C.11) and an effects library (0.2, presets): the host's, or one of its own.
    const bool clipboard = findService<ClipboardServices>(m_policy.services.get()) != nullptr;
    const bool effects = findService<EffectLibraryServices>(m_policy.services.get()) != nullptr;
    if (!clipboard || !effects) {
        std::vector<std::shared_ptr<CommandServices>> parts;
        if (m_policy.services) {
            parts.push_back(m_policy.services);
        }
        if (!clipboard) {
            parts.push_back(std::make_shared<ClipboardServices>());
        }
        if (!effects) {
            parts.push_back(std::make_shared<EffectLibraryServices>());
        }
        m_policy.services = std::make_shared<ServiceBundle>(std::move(parts));
    }
}

CommandResult CommandEngine::failure(CommandError error) const {
    CommandResult result;
    result.error = std::move(error);
    result.revision = m_session.revision();
    return result;
}

CommandResult CommandEngine::execute(const CommandRequest& request, ExecutionControl* control) {
    return run({request}, {}, request.expectedRevision, request.idempotencyKey, request.dryRun, false, control);
}

CommandResult CommandEngine::execute(const CommandBatch& batch, ExecutionControl* control) {
    if (batch.commands.empty()) {
        return failure({ErrorCode::InvalidRequest, "a batch needs at least one command", nullptr});
    }
    for (std::size_t i = 0; i < batch.commands.size(); ++i) {
        const CommandRequest& c = batch.commands[i];
        if (c.expectedRevision || c.idempotencyKey || c.dryRun) {
            return failure({ErrorCode::InvalidRequest,
                            "command " + std::to_string(i) +
                                ": expectedRevision, idempotencyKey and dryRun belong to the batch, not its commands",
                            Json{{"index", i}}});
        }
    }
    const std::string label =
        batch.label.empty() ? "Batch (" + std::to_string(batch.commands.size()) + " commands)" : batch.label;
    return run(batch.commands, label, batch.expectedRevision, batch.idempotencyKey, batch.dryRun, true, control);
}

CommandResult CommandEngine::executeJson(const Json& document, ExecutionControl* control) {
    if (isBatchJson(document)) {
        auto batch = batchFromJson(document);
        return batch ? execute(*batch, control) : failure(std::move(batch.error()));
    }
    auto request = requestFromJson(document);
    return request ? execute(*request, control) : failure(std::move(request.error()));
}

void CommandEngine::remember(const std::string& key, std::string fingerprint, const CommandResult& result) {
    if (m_policy.idempotencyCacheSize == 0) {
        return;
    }
    if (m_idempotency.emplace(key, Remembered{std::move(fingerprint), result}).second) {
        m_idempotencyOrder.push_back(key);
    }
    while (m_idempotencyOrder.size() > m_policy.idempotencyCacheSize) {
        m_idempotency.erase(m_idempotencyOrder.front());
        m_idempotencyOrder.pop_front();
    }
}

CommandResult CommandEngine::run(const std::vector<CommandRequest>& commands, const std::string& label,
                                 std::optional<std::uint64_t> expectedRevision,
                                 const std::optional<std::string>& idempotencyKey, bool dryRun, bool batch,
                                 ExecutionControl* control) {
    // 1. Resolve every command first: unknown names fail before anything else is looked at.
    std::vector<const CommandSpec*> specs;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const CommandRequest& request = commands[i];
        const CommandSpec* spec = m_registry.find(request.command, request.version);
        if (!spec) {
            const CommandSpec* any = m_registry.find(request.command);
            if (any && request.version) {
                return failure(indexed({ErrorCode::UnsupportedVersion,
                                        any->name + " has no version " + std::to_string(*request.version) +
                                            " (newest: " + std::to_string(any->version) + ")",
                                        Json{{"newestVersion", any->version}}},
                                       batch, i));
            }
            return failure(indexed({ErrorCode::UnknownCommand, "unknown command \"" + request.command + "\"",
                                    Json{{"command", request.command}}},
                                   batch, i));
        }
        specs.push_back(spec);
    }

    // 2. Idempotency: a replay returns the first result, whatever happened since.
    std::string fingerprint;
    if (idempotencyKey) {
        Json f{{"batch", batch}, {"label", label}, {"dryRun", dryRun}, {"commands", Json::array()}};
        for (std::size_t i = 0; i < commands.size(); ++i) {
            f["commands"].push_back(
                Json{{"command", specs[i]->name}, {"version", specs[i]->version}, {"params", commands[i].params}});
        }
        fingerprint = f.dump();
        if (const auto it = m_idempotency.find(*idempotencyKey); it != m_idempotency.end()) {
            if (it->second.fingerprint != fingerprint) {
                return failure({ErrorCode::IdempotencyConflict,
                                "idempotency key \"" + *idempotencyKey + "\" was used for a different request",
                                Json{{"idempotencyKey", *idempotencyKey}}});
            }
            CommandResult replay = it->second.result;
            replay.replayed = true;
            return replay;
        }
    }

    // 3. Permissions, then the revision the client based the request on.
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (!m_policy.allowed.has(specs[i]->capability)) {
            return failure(indexed({ErrorCode::PermissionDenied,
                                    specs[i]->name + " needs the " + std::string(toString(specs[i]->capability)) +
                                        " capability",
                                    Json{{"capability", toString(specs[i]->capability)}}},
                                   batch, i));
        }
        if (specs[i]->runsModel && !m_policy.allowModelCommands) {
            return failure(indexed({ErrorCode::PermissionDenied,
                                    specs[i]->name + " runs an AI model, which this client may not start",
                                    Json{{"capability", toString(Capability::MediaAnalysis)}, {"runsModel", true}}},
                                   batch, i));
        }
        if (std::find(m_policy.deniedCommands.begin(), m_policy.deniedCommands.end(), specs[i]->name) !=
            m_policy.deniedCommands.end()) {
            return failure(indexed({ErrorCode::PermissionDenied, specs[i]->name + " is not available to this client",
                                    Json{{"denied", true}, {"touchesFiles", specs[i]->touchesFiles}}},
                                   batch, i));
        }
        if (specs[i]->touchesFiles && !m_policy.allowFileCommands) {
            if (m_policy.fileRoots.empty()) {
                return failure(indexed({ErrorCode::PermissionDenied,
                                        specs[i]->name + " reads or writes files, which this client may not",
                                        Json{{"touchesFiles", true}}},
                                       batch, i));
            }
            if (const auto outside = outsideRoots(*specs[i], commands[i].params, m_policy.fileRoots)) {
                return failure(indexed({ErrorCode::PermissionDenied, *outside, Json{{"touchesFiles", true}, {"confined", true}}},
                                       batch, i));
            }
        }
    }
    const ProjectSnapshot snapshot = m_session.snapshot();
    const std::uint64_t revision = m_session.revision();
    if (expectedRevision && *expectedRevision != revision) {
        return failure({ErrorCode::StaleRevision,
                        "the project is at revision " + std::to_string(revision) + ", not " +
                            std::to_string(*expectedRevision),
                        Json{{"expected", *expectedRevision}, {"current", revision}}});
    }

    // 4. Params against each command's schema (always, also in release builds).
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const std::vector<SchemaIssue> issues = validateAgainstSchema(specs[i]->params, commands[i].params);
        if (!issues.empty()) {
            Json list = Json::array();
            for (const SchemaIssue& issue : issues) {
                list.push_back(Json{{"path", issue.path}, {"message", issue.message}});
            }
            return failure(indexed({ErrorCode::InvalidParams,
                                    specs[i]->name + ": " + issues.front().path + " " + issues.front().message,
                                    Json{{"issues", std::move(list)}}},
                                   batch, i));
        }
    }

    // 5. Run the commands in order on a working state; ids continue from the session's high-water mark so
    //    an undone edit's ids are never reused.
    ProjectState input = *snapshot;
    if (input.ids.next < m_session.idHighWater()) {
        input.ids.next = m_session.idHighWater();
    }
    ProjectState working = input;
    CommandResult result;
    std::vector<std::function<void()>> afterCommit; // host side effects: only once everything succeeded
    ExecutionContext::Stage stage;                  // what they will leave, for the later steps
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const CommandSpec& spec = *specs[i];
        if (control && batch) {
            if (auto stop = control->checkpoint("step " + std::to_string(i + 1) + " of " +
                                                std::to_string(commands.size()) + ": " + spec.name)) {
                return failure(indexed(std::move(*stop), batch, i));
            }
        }
        CommandOutput out;
        try {
            ExecutionContext context(working, commands[i].params, control, m_policy.services.get());
            context.setDryRun(dryRun);
            context.setStage(&stage);
            out = spec.execute(context);
        } catch (const CommandFailure& f) {
            return failure(indexed(f.error, batch, i));
        } catch (const std::exception& e) {
            return failure(indexed({ErrorCode::Internal, spec.name + " failed: " + e.what(), nullptr}, batch, i));
        }
        if (spec.mutates()) {
            if (const auto problem = lockedTrackChange(working, out.state)) {
                return failure(indexed({ErrorCode::LockedTrack, *problem, nullptr}, batch, i));
            }
            working = std::move(out.state);
        }
        for (auto& effect : out.afterCommit) {
            afterCommit.push_back(std::move(effect));
        }
        result.createdIds.insert(result.createdIds.end(), out.createdIds.begin(), out.createdIds.end());
        result.warnings.insert(result.warnings.end(), out.warnings.begin(), out.warnings.end());
        if (batch) {
            result.steps.push_back(StepResult{spec.name, std::move(out.createdIds), std::move(out.data),
                                              std::move(out.warnings)});
        } else {
            result.data = std::move(out.data);
        }
    }

    // 6. Invariants, the ChangeSet, then preview or commit. A result that differs only in node identity
    //    (renaming a clip to its own name) changes nothing and commits nothing.
    bool changed = !isSameRevision(input, working);
    if (changed) {
        if (auto valid = validate(working, Validation::Structure); !valid) {
            return failure({ErrorCode::InvariantViolation, valid.error().message, nullptr});
        }
        result.changes = diff(*snapshot, working);
        changed = !result.changes.empty();
    }
    result.dryRun = dryRun;
    if (dryRun) {
        result.preview = changed ? std::make_shared<const ProjectState>(std::move(working)) : snapshot;
        result.revision = revision;
        return result;
    }
    if (changed) {
        if (control) {
            if (auto stop = control->checkpoint("commit")) {
                return failure(std::move(*stop));
            }
        }
        std::string undoLabel = batch ? label : specs.front()->title;
        if (!m_policy.undoLabelPrefix.empty()) {
            undoLabel = m_policy.undoLabelPrefix + ": " + undoLabel;
        }
        auto committed = m_session.commit(undoLabel, working);
        if (!committed) {
            return failure({ErrorCode::InvariantViolation, committed.error().message, nullptr});
        }
        result.changes = std::move(*committed);
        result.committed = true;
    }
    for (const auto& effect : afterCommit) {
        effect();
    }
    result.revision = m_session.revision();
    if (idempotencyKey) {
        remember(*idempotencyKey, std::move(fingerprint), result);
    }
    return result;
}

} // namespace wabcut::command
