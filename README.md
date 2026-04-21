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

After dependencies are installed, configure the project with:

```bash
cmake -S . -B build
```
