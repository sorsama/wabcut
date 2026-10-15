#pragma once

// The project's transcript edits (model Transcripts.h, C.7) applied on top of a transcript as the analysis cache
// holds it. The cached transcript stays as transcribed, so transcribing again never loses a correction: the next
// SequenceTranscript applies the same edits to the new words.
//
// A correction finds its words by source time: those whose middle lies in its range. They read the corrected text:
// word for word when the counts match (each keeps its timing), else as new words sharing the span by length; empty
// text removes them. When no word lies there any more (re-transcription dropped it), the text comes back as a new
// word over the correction's range. Each application is reported: applied (the words still read what was
// corrected), conflict (they read something else now; the correction still wins), restored (no words were there).

#include <wabcut/ai/Transcript.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wabcut::ai::edit {

enum class CorrectionOutcome : std::uint8_t { Applied, Conflict, Restored };

struct CorrectionReport {
    std::size_t correction = 0; ///< index into TranscriptEdits::corrections
    CorrectionOutcome outcome = CorrectionOutcome::Applied;
    std::string found;          ///< what the words read before the correction (empty when restored)
};

/// `transcript` with `edits`' corrections applied (speakers are not touched: see speakerOf). Words a correction
/// changed are marked edited and approved; segments keep their times and are rebuilt around the words.
Transcript applyCorrections(const Transcript& transcript, const TranscriptEdits& edits,
                            std::vector<CorrectionReport>* report = nullptr);

/// The project speaker of a word: the speaker assigned over its middle, else its diarization label's speaker, else an
/// invalid id. `edits` may be null.
SpeakerId speakerOf(const TranscriptWord& word, const TranscriptEdits* edits);

std::string_view toString(CorrectionOutcome outcome);

} // namespace wabcut::ai::edit
