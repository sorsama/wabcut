# Performance baselines

One file per machine, `<machine id>.json` (the id hashes the CPU, its logical cores, the memory size and the GPU; every
benchmark JSON in `test-output/perf/` names it). The perf benchmarks fail when a gated metric costs more than 20 % over
the value here (and more than its absolute slack), after one retry.

- A machine without a file only records one: `test-output/perf/baseline-candidate.json`.
- To record or refresh this machine's, on a quiet machine: `WABCUT_PERF_UPDATE_BASELINE=1 ctest --preset win-msvc -L perf -R "^Benchmark"`.
  Commit the file with the change that moved the numbers, and say why in the commit message.

See docs/performance.md.
