#pragma once

// The OPUS-MT translation models of the built-in manifest (AI-12): four files per model (encoder, decoder, tokenizer,
// config) in a directory of its own in the store, "opus-mt-<pair>/". Generated: TranslationManifest.cpp.

#include <wabcut/ai/ModelStore.h>

#include <vector>

namespace wabcut::ai {

std::vector<ModelInfo> translationManifest();

} // namespace wabcut::ai
