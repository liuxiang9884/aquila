# GTest Migration Design

## Goal

Migrate every test under `test/` from ad hoc executable-return-code checks to
GoogleTest-based test binaries.

## Scope

- Convert `test/websocket/*.cpp` to `gtest`
- Replace the current `hello_world_smoke_test` with a `gtest` binary
- Remove `ctest` registration from the project for these tests
- Keep the existing per-file test binary layout instead of aggregating tests

## Non-Goals

- Do not merge all tests into one executable
- Do not redesign the test cases beyond converting assertions and harness style
- Do not add `gtest_discover_tests()` or any `ctest` dependency

## Build Integration

- Add `find_package(GTest CONFIG REQUIRED)` in `cmake/third_party.cmake`
- Link every test target against `GTest::gtest_main`
- Remove `enable_testing()` from the top-level `CMakeLists.txt`
- Remove all `add_test(...)` usage from `test/CMakeLists.txt` and
  `test/websocket/CMakeLists.txt`

## Test Binary Layout

Keep one test source file mapped to one executable target:

- `test/hello_world_test.cpp`
- `test/websocket/types_test.cpp`
- `test/websocket/runtime_policy_test.cpp`
- `test/websocket/prepared_write_test.cpp`
- `test/websocket/handshake_test.cpp`
- `test/websocket/frame_codec_test.cpp`
- `test/websocket/tls_socket_test.cpp`
- `test/websocket/critical_session_test.cpp`
- `test/websocket/gate_loopback_integration_test.cpp`

This keeps the current module boundaries and minimizes migration risk.

## Test Style

- Replace manual `return 1` checks with `EXPECT_*` / `ASSERT_*`
- Keep existing helper functions and fake transports where they already exist
- Preserve compile-time `static_assert` checks at namespace scope where
  appropriate
- Use `gtest` as the executable entrypoint instead of handwritten `main()`

## Hello World Smoke Test

The current smoke check depends on executing `aquila_hello_world` and checking
its stdout. The migrated version will stay a smoke test, but it will become a
`gtest` binary that launches the built tool and asserts its output matches the
expected `hello world` line.

## Verification

- `./build.sh debug`
- Run every generated `gtest` binary under `build/debug/test/`
- Confirm the previous websocket coverage remains intact after migration

## Risks And Mitigations

- `GTest` may not yet be part of the local vcpkg environment:
  mitigate by wiring it through `find_package` and updating prerequisite docs
- The hello-world smoke test requires process execution and output capture:
  mitigate by keeping it isolated in its own test binary
- Converting many files at once can hide regressions:
  mitigate by preserving per-file executables and rerunning all test binaries
