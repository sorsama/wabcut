#pragma once

// IrDocument -> ProjectState, through the model's own operations (ProjectOps/EditOps), so the result is
// valid by construction; validate(Strict) runs at the end as a guard.
//
// This is the one place that knows what the model can and cannot hold. Things it cannot hold yet are
// reported here, each behind its own function so the model's rev-2 additions slot in locally:
//  * transitions        -> importTransition()       (imported as cuts: the model has Transition edges but
//                                                    no operation that creates them yet)
//  * explicit duration  -> applySequenceDuration()  (trailing gaps kept with edit::setSequenceDuration)
//  * clip speed         -> reported by the parsers  (no operation creates retimed clips yet)
//  * media locator      -> makeMediaItem()          (absolute + project-relative path, offline state)

#include "Ir.h"

#include <wabcut/interchange/Import.h>

#include <expected>
#include <filesystem>
#include <string>

namespace wabcut::interchange::detail {

struct BuildContext {
    Format format = Format::Unknown;
    /// File label used in report locations of builder entries.
    std::string fileLabel;
    /// Name of the bin that receives the imported media and sequences.
    std::string binName;
    /// Folder relative media references resolve against.
    std::filesystem::path baseDirectory;
    /// Project file MediaLocator::relativePath is computed against (ImportOptions::projectFile, or a file in
    /// baseDirectory).
    std::filesystem::path projectFile;
    const ImportOptions* options = nullptr;
};

std::expected<ImportResult, Error> buildProject(IrDocument document, const BuildContext& context);

} // namespace wabcut::interchange::detail
