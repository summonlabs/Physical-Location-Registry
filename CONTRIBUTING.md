# Contributing to Physical Location Registry

Thanks for contributing. Physical Location Registry is Apache-2.0 licensed and
accepts contributions from individuals and organizations.

## License and contributions

By submitting a contribution you agree that your contribution is offered under
the Apache License 2.0, and you warrant that you have the right to do so. No
Contributor License Agreement (CLA) is required.

## How to contribute

1. Open an issue, or choose an existing one, describing the change you intend
   to make.
2. Make your changes on a feature branch off `main`.
3. Keep the change focused and consistent with the existing style and design.
4. Build and run the test suite locally before opening a pull request.
5. Describe the completed work accurately in the imperative tense; keep history
   linear and do not squash away implementation history.

## Commit trailers

Commits in this repository carry the configured Git author only. Do not add
`Co-authored-by`, `Signed-off-by` or generated-by trailers, and do not add
AI or tool attribution of any kind.

## Quality expectations

- C++20, built with the project CMake options; the library, tools, examples,
  benchmarks and tests must compile warning-clean under the configured warning
  flags (MSVC /W4 /WX, GCC and Clang -Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Werror).
- New behavior must be covered by tests and pass the full test suite.
  Determinism, generation fencing, alias conflict, move atomicity and
  persistence recovery claims need a test that would fail if the guarantee
  regressed.
- Do not add timeouts, watchdogs or forced termination to tests. A hanging test
  is a defect in the code under test, not a scheduling accident.
- Do not introduce third-party dependencies into the runtime. The library is
  deliberately dependency-free: the C++ standard library plus the operating
  system interfaces needed for durable publication and writer fencing.
- Documentation must be updated for public API or behavior changes.
- No telemetry transmission: all state is written to local files chosen by the
  operator and never leaves the machine running the software.

## Honest reporting

Documentation, tests and commit messages must distinguish behavior that is
implemented and measured from behavior that is synthetic, simulated or
unsupported. Benchmark results must be reproducible and must state the
methodology, the hardware context and the fact that they measure completed
useful work rather than submission latency.

## Review

Maintainers review for correctness, determinism, safety, and honest reporting.
