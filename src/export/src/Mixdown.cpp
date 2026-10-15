#include "Mixdown.h"

namespace wabcut::exporter::detail {

namespace {

playback::OfflineMixdownOptions mixOptions(const EditSession& session) {
    playback::OfflineMixdownOptions options;
    options.redactions = playback::AudioRedactions::of(*session.snapshot()); // bleeps as in the preview (I.6)
    options.takes = playback::AudioTakes::of(*session.snapshot());           // enhanced speech as in the preview (A.4)
    options.project = session.snapshot();                                  // nested and multicam clips (C.8)
    return options;
}

} // namespace

OfflineMix::OfflineMix(ProjectState project, SequencePtr sequence, int channels, int sampleRate, Ticks start)
    : m_session(std::move(project)), m_feed(m_session),
      m_mix(std::move(sequence), channels, sampleRate, start, m_feed, mixOptions(m_session)) {}

} // namespace wabcut::exporter::detail
