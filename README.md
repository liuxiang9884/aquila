# aquila

## Build Prerequisites

This project expects `vcpkg` to be installed under the current user's home directory:

```text
$HOME/vcpkg
```

`cmake/settings.cmake` resolves the toolchain from:

```text
$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
```

To install the packages required by `cmake/third_party.cmake` on Linux:

```bash
$HOME/vcpkg/vcpkg install \
  fmt \
  quill \
  tomlplusplus \
  cli11 \
  gtest \
  magic-enum \
  vincentlaucsb-csv-parser \
  yyjson \
  nameof \
  drogon \
  fast-float \
  benchmark \
  abseil \
  --triplet x64-linux
```

After dependencies are installed, build the debug tree with:

```bash
./build.sh debug
```

The test binaries under `test/` are built with GoogleTest. Run them directly
from `build/debug/test/` and use `--gtest_filter=...` when you want to narrow a
suite to a subset of cases.

## WebSocket Microbenchmarks

Build the optimized tree before taking timing numbers:

```bash
./build.sh release
```

The minimal WebSocket HFT slice builds the targeted benchmark binaries with
Google Benchmark:

```bash
cmake --build build/release --target \
  prepared_write_benchmark \
  frame_codec_benchmark \
  active_spin_benchmark \
  session_write_path_benchmark \
  session_read_path_benchmark \
  runtime_loopback_benchmark \
  affinity_policy_comparison_benchmark \
  cold_path_handshake_benchmark -j8
```

You can run them directly from `build/release/benchmark/websocket/`. Each binary
prints Google Benchmark timing columns plus custom counters for
`min_ns/p50_ns/p99_ns/p999_ns/max_ns` and runtime labels such as CPU affinity,
scheduling policy, TLS mode, and endpoint class.

These are component-level microbenchmarks for the single-connection GateIO
client stack:
- `prepared_write_benchmark`: prepared-write arena acquire/release latency
- `frame_codec_benchmark`: client-side frame encoding latency, including mask-key
  RNG cost
- `active_spin_benchmark`: one owner-loop iteration in the active spin runtime

The first real-I/O benchmark layer runs against a nonblocking local
`socketpair()` harness:
- `session_write_path_benchmark`: `CommitPreparedWrite()` through
  `CriticalSession::DriveWrite()` into a live kernel socket buffer
- `session_read_path_benchmark`: peer frame write through
  `CriticalSession::DriveRead()` and consumer delivery
- `runtime_loopback_benchmark`: `ActiveSpinLoop + CriticalSession` message
  latency with one message in flight over the local socketpair harness
- `affinity_policy_comparison_benchmark`: sequentially compares baseline,
  pinned, prefaulted, and memory-locked runtime policies on the same local
  socketpair loopback path, skipping variants that cannot apply in the current
  environment
- `cold_path_handshake_benchmark`: full local loopback
  `TCP connect + TLS handshake + WebSocket handshake` cold-path timing with TLS
  session resumption disabled

These benchmarks are still not end-to-end GateIO `wss` latency reports and
should not be used to claim full socket/TLS/handshake path latency.

This WebSocket slice is intentionally specialized for one low-latency GateIO
`wss` connection. `Layer 1` is a user-driven core for handshake, TLS/WS state,
prepared writes, and `DriveRead()` / `DriveWrite()` advancement. `Layer 2` is a
thin wrapper that can apply one owner thread, CPU affinity, and an active spin
runtime without changing the core API.

For low-jitter runs, pin the owner thread, keep the process resident with
`mlockall`, prefault hot stacks/buffers before entering the steady-state loop,
and only enable real-time scheduling if the host is already isolated for this
path.

For live GateIO handshake validation:

```bash
timeout 10s ./build/debug/tools/websocket_probe \
  --host fx-ws.gateio.ws \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

The probe is a cold-path to active-state validation tool. It reports connection
state transitions, any surfaced error code, and a final metric summary, but it
is still not a long-run health monitor.
