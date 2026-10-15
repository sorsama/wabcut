# wabcut_interchange

Pure C++23 (no Qt): one-click import of Final Cut Pro 7 XML (Premiere/Resolve "FCP XML"), FCPXML 1.9–1.11,
OpenTimelineIO JSON and CMX3600 EDLs, plus an experimental Premiere `.prproj` reader. The support matrix,
format notes and the `.prproj` findings are in [`docs/interchange.md`](../../docs/interchange.md).

| Header | What it gives you |
|---|---|
| `Import.h` | `ImportRouter::detect` / `importFile` / `importData`, `ImportOptions`, `ImportResult`, `UnresolvedMedia` |
| `ImportReport.h` | carried / approximated / dropped entries with category and source location |
| `MediaRelinker.h` | exact-path, project-relative and search-root relinking; optional `MediaInspector` |
| `FileUrl.h` | `file://` URL and path decoding as Premiere, FCP and OTIO write them |
| `ImportSummary.h` | the text and JSON views `wabcut-cli import` prints |

Private layout (`src/`): one parser per format (`Fcp7Importer`, `FcpXmlImporter`, `OtioImporter`,
`EdlImporter`, `PremiereProject`) producing the intermediate representation in `Ir.h`; `TimelineBuilder`
turns it into a `ProjectState` through the model's edit operations and is the only place that knows what the
model can hold (transitions and retimes are reported there until the model can create them);
`TimeMath` converts interchange times exactly with Time.h's checked 128-bit helpers.

Tests: `tests/interchange/` (fixtures in `fixtures/`, real Premiere projects with ground truth in
`fixtures/prproj/`). Run them with `ctest --preset win-msvc -R interchange`.
