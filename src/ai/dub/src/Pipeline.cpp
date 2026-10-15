#include <wabcut/ai/dub/Pipeline.h>

#include <wabcut/ai/Generation.h>
#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/Sha256.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace wabcut::ai::dub {

namespace {

std::string utcNow() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

Ticks ticksOfFrames(std::size_t frames, int rate) {
    return Ticks{static_cast<std::int64_t>(frames) * kTicksPerSecond / rate};
}

/// `outcome` once the cloned voice `voiceId` (none: empty) is removed from the provider's account, whatever happened (not
/// cancellable). A failed deletion is on the run's warnings, or on the error's message when the run failed: the voice
/// is the speaker's biometric data and the user deletes it by hand then (review wave 5, A5-P2-14 - a destructor guard
/// wrote its warning into the run after it had been moved out, and lost it on every error).
std::expected<DubRun, AiError> withVoiceDeleted(ISpeechSynth& speech, const std::string& voiceId, std::expected<DubRun, AiError> outcome) {
    if (voiceId.empty()) {
        return outcome;
    }
    auto deleted = speech.deleteVoice(voiceId, std::stop_token{});
    if (deleted) {
        return outcome;
    }
    const std::string warning =
        "the cloned voice could not be deleted from the provider's account (" + deleted.error().message + "): delete it there by hand";
    if (outcome) {
        outcome->warnings.push_back(warning);
    } else {
        outcome.error().message += "; " + warning;
    }
    return outcome;
}

} // namespace

std::size_t characterCount(std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

CostEstimate estimateDubCost(const ModelSpec& model, std::span<const DubLine> lines, std::chrono::year_month_day date) {
    CostEstimate total;
    total.priced = true;
    for (const DubLine& line : lines) {
        const CostEstimate e = estimateSpeechCost(model, static_cast<std::int64_t>(characterCount(line.text)), date);
        total.expected += e.expected;
        total.ceiling += e.ceiling;
        total.inputTokens += e.inputTokens;
        total.audioMilliseconds += e.audioMilliseconds;
        total.priced = total.priced && e.priced;
    }
    return total;
}

std::vector<float> trimSilence(std::span<const float> samples, int rate) {
    constexpr float threshold = 0.00316f; // -50 dBFS
    std::size_t first = 0;
    while (first < samples.size() && std::abs(samples[first]) < threshold) {
        ++first;
    }
    if (first == samples.size()) {
        return {};
    }
    std::size_t last = samples.size();
    while (last > first && std::abs(samples[last - 1]) < threshold) {
        --last;
    }
    const std::size_t pad = static_cast<std::size_t>(rate / 50); // 20 ms
    first = first > pad ? first - pad : 0;
    last = std::min(samples.size(), last + pad);
    return {samples.begin() + static_cast<std::ptrdiff_t>(first), samples.begin() + static_cast<std::ptrdiff_t>(last)};
}

namespace {

/// The lines spoken in `voice`, fitted and written to one file: runDub after the voice is ready.
std::expected<DubRun, AiError> speakLines(const DubJob& job, ISpeechSynth& speech, SpeechAudio& audio, std::stop_token stop,
                                          const LineProgress& progress, DubRun run, const SpeechRequest& voice) {
    // 1. Every line, generated and decoded.
    const auto date = todayUtc();
    MicroUsd worstCase = 0; // what the lines sent so far could cost at most: the cap holds against it
    std::vector<std::vector<float>> spoken;
    for (std::size_t i = 0; i < job.lines.size(); ++i) {
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "stopped after " + std::to_string(i) + " lines"));
        }
        const DubLine& line = job.lines[i];
        const std::size_t characters = characterCount(line.text);
        const CostEstimate cost = estimateSpeechCost(speech.spec(), static_cast<std::int64_t>(characters), date);
        if (job.budget && worstCase + cost.ceiling > *job.budget) {
            return std::unexpected(AiError(AiErrorCode::SpendCapExceeded,
                                           "line " + std::to_string(i + 1) + " could take the run past its cap: stopped after " +
                                               std::to_string(i) + " lines (raise the cap or shorten the text)"));
        }
        SpeechRequest request = voice;
        request.text = line.text;
        auto result = speech.synthesize(request, stop);
        if (!result) {
            AiError error = result.error();
            error.message = "line " + std::to_string(i + 1) + ": " + error.message;
            return std::unexpected(std::move(error));
        }
        run.estimated += cost.expected;
        worstCase += cost.ceiling;
        run.characters += characters;
        auto decoded = audio.decode(*result, job.rate);
        if (!decoded) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "line " + std::to_string(i + 1) + ": the speech cannot be decoded (" +
                                                                           decoded.error().message + ")"));
        }
        std::vector<float> samples = trimSilence(*decoded, job.rate);
        if (samples.empty()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "line " + std::to_string(i + 1) + ": the provider sent silence"));
        }
        run.generated.push_back(ticksOfFrames(samples.size(), job.rate));
        spoken.push_back(std::move(samples));
        if (progress) {
            progress(i + 1, job.lines.size());
        }
    }

    // 2. Where they go, and how fast.
    std::vector<Slot> targets;
    if (job.kind == DubKind::Narration) {
        NarrationLayout layout = layoutNarration(job.lines, run.generated, job.at, job.gap, job.beats);
        if (!layout.warning.empty()) {
            run.warnings.push_back(layout.warning);
        }
        targets = std::move(layout.targets);
    } else {
        for (const DubLine& line : job.lines) {
            targets.push_back(Slot{line.start, line.duration});
        }
    }
    run.fitted = fitLines(targets, run.generated, job.fit, &run.report);

    // 3. One file: the lines one after another, 50 ms apart.
    std::vector<float> file;
    const std::size_t spacing = static_cast<std::size_t>(job.rate / 20);
    for (std::size_t i = 0; i < spoken.size(); ++i) {
        std::vector<float> samples = run.fitted[i].stretched ? audio.stretch(spoken[i], job.rate, run.fitted[i].speed) : std::move(spoken[i]);
        const std::size_t offset = file.size();
        file.insert(file.end(), samples.begin(), samples.end());
        const TimeRange segment{ticksOfFrames(offset, job.rate), ticksOfFrames(samples.size(), job.rate)};
        run.fitted[i].duration = segment.duration; // what the stretcher made
        run.audio.segments.push_back(segment);
        file.insert(file.end(), spacing, 0.0f);
    }

    std::string text;
    for (const DubLine& line : job.lines) {
        text += line.text;
        text += '\n';
    }
    const std::string operation = job.kind == DubKind::Dub ? "dub" : "narration";
    // Named by the samples it holds (review wave 5, A5-P1-6): the same lines spoken again sound different (the provider
    // is not deterministic, the time fit depends on the cues), and an earlier run's clips address its file by segment
    // offsets - so a re-run never touches it. An existing file is never replaced: same samples or not, the new one
    // gets a name of its own.
    Sha256 content;
    content.update(std::span(reinterpret_cast<const std::byte*>(file.data()), file.size() * sizeof(float)));
    content.update(std::to_string(job.rate));
    const std::string stem = operation + "-" + content.finishHex().substr(0, 16);
    std::error_code ec;
    std::filesystem::create_directories(job.directory, ec);
    std::filesystem::path path = job.directory / (stem + ".wav");
    for (int n = 2; std::filesystem::exists(path, ec); ++n) {
        path = job.directory / (stem + "-" + std::to_string(n) + ".wav");
    }
    auto item = audio.write(path, file, job.rate);
    if (!item) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the speech file cannot be written: " + item.error().message));
    }
    item->name = job.name.empty() ? operation : job.name;
    GeneratedProvenance provenance;
    provenance.provider = job.provider.empty() ? speech.spec().provider : job.provider;
    provenance.model = speech.spec().id;
    provenance.promptHash = promptHash(text);
    provenance.createdAt = job.createdAt.empty() ? utcNow() : job.createdAt;
    provenance.kind = "audio";
    provenance.operation = operation;
    provenance.source = job.source;
    item->generated = provenance;
    if (auto sidecar = writeProvenanceSidecars(path, provenance); !sidecar) {
        run.warnings.push_back("no provenance sidecar next to the file: " + sidecar.error().message);
    }
    run.audio.item = std::move(*item);
    return run;
}

} // namespace

std::expected<DubRun, AiError> runDub(const DubJob& job, ISpeechSynth& speech, SpeechAudio& audio, std::stop_token stop,
                                      const LineProgress& progress) {
    const auto invalid = [](std::string message) { return std::unexpected(AiError(AiErrorCode::InvalidRequest, std::move(message))); };
    if (job.lines.empty()) {
        return invalid("nothing to say: no line has words");
    }
    if (job.rate < 8000 || job.rate > 192000) {
        return invalid("unsupported sample rate");
    }
    DubRun run;
    run.lines = job.lines;
    SpeechRequest voice = job.voice;
    std::string clonedId;
    if (!job.cloneSamples.empty()) {
        auto cloned = speech.cloneVoice(job.cloneSamples, job.name, stop);
        if (!cloned) {
            return std::unexpected(cloned.error());
        }
        clonedId = cloned->voiceId;
        if (cloned->requiresVerification) {
            return withVoiceDeleted(speech, clonedId,
                                    std::unexpected(AiError(AiErrorCode::InvalidRequest,
                                                            "the provider wants the speaker to verify the cloned voice first: nothing was generated")));
        }
        voice.voiceId = cloned->voiceId;
        run.cloned = true;
    }

    return withVoiceDeleted(speech, clonedId, speakLines(job, speech, audio, stop, progress, std::move(run), voice));
}

} // namespace wabcut::ai::dub
