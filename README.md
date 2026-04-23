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
scheduling policy, TLS mode, and endpoint class. These are targeted local
microbenchmarks for the single-connection GateIO `wss` path, not full
exchange-to-strategy latency reports.
