#include "TranscriptBridge.h"

#include <wabcut/ai/AudioEnergy.h>
#include <wabcut/ai/edit/MediaServices.h>

#include <algorithm>

namespace wabcut::app {
namespace {
// review() calls prefetch once with all candidate ranges. Decoding and filesystem access stay on this worker;
// individual rmsDb calls only read the resulting memory cache. Cooperative cancellation reaches the decoder.
class ReviewEnergy final : public ai::edit::AudioEnergyProbe {
public:
    ReviewEnergy(std::filesystem::path project, std::stop_token stop, std::function<void(float)> progress)
        : m_project(std::move(project)), m_stop(stop), m_progress(std::move(progress)) {}
    void prefetch(std::span<const Range> ranges) const override {
        std::map<std::pair<std::string, int>, std::vector<TimeRange>> groups;
        for (const auto& r : ranges) {
            if (m_stop.stop_requested())
                return;
            if (!r.media)
                continue;
            if (const auto path = ai::edit::mediaFilePath(*r.media, m_project))
                groups[{*path, ai::edit::audioContainerStream(*r.media, r.audioStream)}].push_back(r.source);
        }
        std::size_t done = 0;
        for (const auto& [key, spans] : groups) {
            if (m_stop.stop_requested())
                return;
            const auto result = m_service.run(
                ai::EnergyRequest{key.first, key.second, spans, false}, m_stop, [&](float fraction) {
                    m_progress((static_cast<float>(done) + fraction) / static_cast<float>(groups.size()));
                });
            (void)result; // an unavailable range remains unmeasured; review retains its uncertainty
            ++done;
        }
    }
    std::optional<double> rmsDb(const MediaItem& media, int stream, TimeRange range) const override {
        if (m_stop.stop_requested())
            return {};
        const auto path = ai::edit::mediaFilePath(media, m_project);
        if (!path)
            return {};
        const auto known = m_service.cached(*path, ai::edit::audioContainerStream(media, stream), range);
        return known ? std::optional<double>(known->rmsDb) : std::nullopt;
    }

private:
    std::filesystem::path m_project;
    std::stop_token m_stop;
    std::function<void(float)> m_progress;
    mutable ai::AudioEnergyService m_service;
};
} // namespace

void TranscriptBridge::cancelReview() {
    ++m_reviewGeneration;
    for (auto& worker : m_reviewWorkers)
        worker.thread.request_stop();
    // Joining only finished workers never waits for decoding on the GUI thread.
    std::erase_if(m_reviewWorkers, [](const ReviewWorker& worker) { return worker.finished->load(); });
    if (m_reviewBusy) {
        m_reviewBusy = false;
        m_reviewProgress = 0;
        emit reviewProgressChanged();
    }
}

void TranscriptBridge::refreshReview() {
    cancelReview();
    cancelPreview();
    m_review.clear();
    m_reviewRemove.clear();
    emit reviewChanged();
    if (!m_controller || !m_mapped || m_transcript.empty())
        return;
    ai::edit::ReviewOptions options;
    options.fillers = m_reviewFillers;
    options.silenceThreshold =
        m_silenceSeconds > 0
            ? std::optional<Ticks>(Ticks{static_cast<std::int64_t>(m_silenceSeconds * kTicksPerSecond)})
            : std::nullopt;
    const auto generation = m_reviewGeneration;
    const auto snapshot = m_mapped;
    const auto transcript = m_transcript;
    const auto project = m_controller->projectFile();
    const auto selection = m_controller->selection();
    const auto sequence = m_controller->currentSequenceId();
    const auto runner = m_reviewRunner;
    const QPointer<TranscriptBridge> self(this);
    m_reviewBusy = true;
    m_reviewProgress = 0;
    emit reviewProgressChanged();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::jthread thread([this, self, generation, snapshot, transcript, project, selection, sequence, options,
                         runner, finished](std::stop_token stop) {
        const auto progress = [this, self, generation](float value) {
            QMetaObject::invokeMethod(
                this,
                [self, generation, value] {
                    if (!self || generation != self->m_reviewGeneration)
                        return;
                    self->m_reviewProgress = std::clamp(static_cast<double>(value), 0.0, 1.0);
                    emit self->reviewProgressChanged();
                },
                Qt::QueuedConnection);
        };
        std::vector<ai::edit::ReviewItem> result;
        QString error;
        try {
            if (!stop.stop_requested()) {
                if (runner)
                    result = runner(transcript, *snapshot, options, stop, progress);
                else {
                    ReviewEnergy probe(project, stop, progress);
                    result = ai::edit::review(transcript, *snapshot, options, &probe);
                }
            }
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        const bool cancelled = stop.stop_requested();
        QMetaObject::invokeMethod(
            this,
            [self, generation, snapshot, selection, sequence, result = std::move(result), error, cancelled] {
                if (!self || generation != self->m_reviewGeneration || !self->m_controller)
                    return;
                self->m_reviewBusy = false;
                self->m_reviewProgress = cancelled ? 0 : 1;
                emit self->reviewProgressChanged();
                if (cancelled || self->m_mapped != snapshot || self->m_controller->snapshot() != snapshot ||
                    self->m_controller->selection() != selection ||
                    self->m_controller->currentSequenceId() != sequence)
                    return;
                if (!error.isEmpty()) {
                    self->setMessage(error);
                    return;
                }
                self->m_review = result;
                for (const auto& item : result)
                    self->m_reviewRemove.push_back(item.confidence >= 0.5f);
                emit self->reviewChanged();
            },
            Qt::QueuedConnection);
        finished->store(true);
    });
    m_reviewWorkers.push_back({std::move(finished), std::move(thread)});
}
} // namespace wabcut::app
