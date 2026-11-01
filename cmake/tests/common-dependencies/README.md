# Common dependency checks

The Host owns its GoogleTest submodule at `third-party/googletest`. It is pinned
to 1.17.0 (`52eb8108c5bdec04579160ae17225d66034bd723`), preserving the version
used before the lizardbyte-common update. Dependabot proposes changes to this
pin separately. The nested GoogleTest pin in lizardbyte-common is not a Host
build input; no local edits or fork of that upstream dependency are required.

`cmake/dependencies/lizardbyte_common.cmake` creates the GoogleTest targets
before the helper library when `BUILD_TESTS=ON`. Both `test_sunshine` and the
helper's test support consume those targets and their include paths. With
`BUILD_TESTS=OFF`, neither GoogleTest nor test support is configured.

This standalone fixture exercises that production wiring without the Host's
capture/encoder SDKs. It checks runtime helper linkage, the selected GoogleTest
source directory, shared output-capture fixtures, and the absence of test
targets in the release configuration. It does not replace a full Host build
or hardware qualification.

From the Host repository root, with CMake 3.24+ and a C++23 compiler:

```sh
git submodule update --init third-party/lizardbyte-common third-party/googletest
cmake -S cmake/tests/common-dependencies -B cmake-build-common-tests -DBUILD_TESTS=ON
cmake --build cmake-build-common-tests --parallel 4
ctest --test-dir cmake-build-common-tests --output-on-failure
cmake -S cmake/tests/common-dependencies -B cmake-build-common-runtime -DBUILD_TESTS=OFF
cmake --build cmake-build-common-runtime --parallel 4
ctest --test-dir cmake-build-common-runtime --output-on-failure
```

Also reconfigure the first build directory with `BUILD_TESTS=OFF`, then `ON`,
building and running CTest each time. This catches stale cache options enabling
test support in a subsequent release build.

When lizardbyte-common's Python dependencies change, refresh the **Host's**
`uv.lock`, not only the dependency's lockfile. Python 3.14+ is required:

```sh
git submodule update --init third-party/glad
uv lock --python 3.14
uv lock --check --python 3.14
uv sync --locked --group glad --python 3.14
```

The update to `f9d91e1d29b7473f58e43acde4579da4e56c4abe` requires
clang-format 23.1.0. Lock refresh should change only that package and its
dependency metadata, not upgrade unrelated tooling.
