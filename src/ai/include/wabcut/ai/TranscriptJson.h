#pragma once

// Versioned JSON form of a Transcript (also the payload of its AnalysisCache entry and sidecar).
//
//   {
//     "schema": "wabcut.transcript", "version": 1, "ticksPerSecond": 254016000000,
//     "language": "en",
//     "source": {"fingerprint": "00c0ffee00c0ffee", "stream": 1, "path": "C:/media/interview.mov"},
//     "provenance": {"engine": "whisper.cpp", "engineVersion": "1.9.5", "modelId": "base",
//                    "modelVersion": "<sha-256>", "params": {"language": "auto", ...}, "createdAt": "..."},
//     "segments": [{"t0": 0, "t1": 508032000000, "firstWord": 0, "wordCount": 2, "text": "Hello world",
//                   "speaker": 0}],
//     "words": [{"text": "Hello", "t0": 0, "t1": 254016000000, "confidence": 0.93, "speaker": 0,
//                "edited": false, "approved": false}, ...]
//   }
//
// Times are integer ticks (model/Time.h), written as JSON integers. The fingerprint is 16 hex digits (a 64-bit
// value does not survive JavaScript numbers). Optional members ("speaker", "path") are omitted when unknown.
// Readers ignore unknown members, reject another schema or version (VersionMismatch) and check the result
// with Transcript::validate().

#include <wabcut/ai/Transcript.h>

#include <expected>
#include <string>
#include <string_view>

namespace wabcut::ai {

inline constexpr std::string_view kTranscriptSchema = "wabcut.transcript";

/// `indent` < 0: compact single line; otherwise pretty-printed with that many spaces.
std::string toJson(const Transcript& transcript, int indent = -1);

/// Errors: ParseError (not JSON, wrong types, missing members), VersionMismatch, InvalidArgument (validate()).
std::expected<Transcript, Error> transcriptFromJson(std::string_view json);

} // namespace wabcut::ai
