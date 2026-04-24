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

The minimal WebSocket HFT slice builds three targeted microbenchmark binaries:

```bash
cmake --build build/release --target \
  prepared_write_benchmark \
  frame_codec_benchmark \
  active_spin_benchmark -j8
```

You can run them directly from `build/release/benchmark/websocket/`. Each binary
prints `min/p50/p99/p99.9/max` plus runtime metadata such as CPU affinity,
scheduling policy, TLS mode, and endpoint class.

These are component-level microbenchmarks for the single-connection GateIO
client stack:
- `prepared_write_benchmark`: prepared-write arena acquire/release latency
- `frame_codec_benchmark`: client-side frame encoding latency, including mask-key
  RNG cost
- `active_spin_benchmark`: one owner-loop iteration in the active spin runtime

They are not end-to-end GateIO `wss` latency reports and should not be used to
claim full socket/TLS/handshake path latency.

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
