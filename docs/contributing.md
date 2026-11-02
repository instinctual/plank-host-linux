# Contributing to the PLANK Linux Host

Develop against the parent [PLANK repository](https://github.com/instinctual/plank),
which pins this subtree and owns product builds, packaging and integration tests.
Read its `AGENTS.md`, `HANDOFF.md` and release build runbook before changing code.

Follow the existing C++ style and `.clang-format`. Preserve upstream attribution
and license notices. Add focused tests for changed behavior, including invalid
input and cleanup paths. Keep protocol changes synchronized with the Client.

The Host has no WebUI, tray, external shaders or localization pipeline.
Configuration, system integration and display EDIDs live in the parent repository.
Do not restore the retired upstream asset-packaging or localization tasks.

GoogleTest sources remain in `tests/`; the full test executable is
`tests/test_sunshine` in the configured build directory. The parent hosted build
also runs production-source input and core fixtures without a GPU. Package
builds use `BUILD_TESTS=OFF`; hardware capture/encode/input qualification is a
separate gate and must be reported separately from compilation.
