# Parser fuzz harnesses

One harness per untrusted input format (C.10). Each defines `LLVMFuzzerTestOneInput`. It returns normally for an
input the parser refuses, and aborts through `WABCUT_FUZZ_CHECK` when an invariant breaks: a parsed result fails
validation, a round trip changes it, or a rejected command changed the project.

## Harnesses

| Harness | Input | Owner | Invariants beyond "no crash, no hang, bounded memory" |
|---|---|---|---|
| `project_io` | `.wabcut` project file | core | structurally valid; save → load gives the same project, and the same text |
| `history` | first byte `0`: `index.json`; `1`: an object file; `2`: a revision's chunks | core | ids increasing; object and chunk round trips |
| `review_package` | a review package's `notes.json` | core | `validate(Strict)`; importing it again changes nothing |
| `clipboard` | a clipboard document (C.11) | core | JSON round trip; pasting it (overwrite, insert) gives `validate(Strict)` or a refusal |
| `graphic` | first byte `0`: a clip's graphic; `1`: a graphic command's layer; `2`: a brand kit; `3`: a template library file (C.13) | core | passes `graphicProblem` / `brandKitProblem` / `templateProblem`; JSON round trip; the draw list at any time has finite numbers |
| `extension` | an extension's manifest, a scripthost protocol line, a prompt form and its answers, the permission file (C.12) | core | manifests and messages parse again from their own output to the same thing (sanitising is idempotent); a form's defaults fit it; the permission file round-trips |
| `command` | a command request or batch (JSON) | core | refused → project unchanged; accepted → `validate(Strict)` and it saves; the result serialises |
| `mcp` | up to 16 JSON-RPC lines | core | every reply is JSON-RPC 2.0; the project stays `validate(Strict)` |
| `fcp7`, `fcpxml`, `otio`, `edl`, `prproj` | another editor's timeline | core (interchange) | `validate(Strict)` (the importer's promise); `.wabcut` round trip |
| `lottie` | a Lottie (Bodymovin JSON) animation (C.13) | core (interchange) | no `internal:` error; the graphic passes `graphicProblem`, round-trips through the graphic JSON and resolves to finite draw lists |
| `kys` | Premiere keyboard shortcuts | keymap | harness only |
| `cube`, `cdl` | `.cube` LUT, ASC CDL `.cc` / `.ccc` / `.cdl` | colour | write → parse → write is stable |
| `ai_sse` | an AI provider's response body | AI platform | SSE / NDJSON decoding does not depend on how the bytes arrive; four chat adapters; error bodies |

The `command` and `mcp` harnesses serve the catalog without the commands that read or write files named in their
parameters (`FuzzRegistry.h`). The importers never look up media paths on disk (`ImportOptions::fileExists`).

## Seeds

Seeds come from three places:
- small files in `seeds/<harness>/`;
- the test fixtures of the format's own suite, copied into `<build>/fuzz/staged/<harness>` at configure time
  (libFuzzer takes folders only);
- generated corpora: `wabcut_fuzz_seeds <build>/fuzz/seeds` writes `.wabcut` files, history files, review packages,
  one request per catalog command and the same requests as MCP sessions, all from the every-shape project in
  `tests/model/ScaleProject.h`. ctest runs it first, as fixture `fuzz_seeds`.

A crash that was fixed keeps its minimized reproducer as `seeds/<harness>/regression-<what>`. A finding in another
owner's parser stays out of the seeds until its fix lands, so the label stays green. The C.10 campaign found two in
the colour module: a `.cube` value that overflows `float` is written back as `inf`, and a tiny CDL power is written
as `0.000000`.

## Running

Every build runs the harnesses in ctest (label `fuzz`): each input once, then `WABCUT_FUZZ_REGRESSION_RUNS`
deterministic mutations (default 1000).
- Without `WABCUT_FUZZ`, `FuzzMain.cpp` is the driver: no coverage feedback, libFuzzer's flags.
- In the `win-msvc-asan` preset (docs/building.md, "Sanitizers and fuzzing") they are libFuzzer executables under
  AddressSanitizer. Explore with them:

```powershell
. .\tools\dev-env.ps1 -Core
cmake --preset win-msvc-asan; cmake --build --preset win-msvc-asan --target fuzz_project_io wabcut_fuzz_seeds
build\win-msvc-asan\bin\wabcut_fuzz_seeds.exe build\win-msvc-asan\fuzz\seeds
build\win-msvc-asan\bin\fuzz_project_io.exe -max_total_time=600 -timeout=10 -rss_limit_mb=2048 -max_len=65536 `
    -artifact_prefix=build\win-msvc-asan\fuzz\artifacts\project_io\ `
    build\win-msvc-asan\fuzz\corpus\project_io build\win-msvc-asan\fuzz\seeds\project_io
```

The first folder is the working corpus: libFuzzer adds the inputs it finds there. Never pass a source folder
first. Run one fuzzer at a time on a machine that is also building.
