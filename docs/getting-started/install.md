# Installation

## Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| C compiler | C11 support | GCC 7+, Clang 5+, Apple Clang |
| CMake | 3.15+ | Build system |
| CivetWeb | 1.16+, with WebSocket support | Required for `MRTK_WITH_RPC=ON` (default); system package |
| yyjson | 0.11+ | Required for RPC; CMake fetches 0.13.0 if no suitable package is found |
| LAPACK | any | Hardware-accelerated matrix operations |

### Platform-Specific Dependencies

=== "Ubuntu / Debian"

    ```bash
    sudo apt-get update
    sudo apt-get install -y cmake build-essential liblapack-dev libcivetweb-dev
    ```

=== "macOS (Homebrew)"

    ```bash
    # Xcode command line tools provide Apple Clang + Accelerate (LAPACK)
    xcode-select --install
    brew install cmake civetweb
    ```

=== "macOS (Apple Silicon)"

    No additional LAPACK installation needed — macOS ships with the Accelerate framework which provides LAPACK.

For MSYS2 UCRT64, the CivetWeb package is
`mingw-w64-ucrt-x86_64-civetweb`. With vcpkg, install `civetweb`.
The RPC implementation currently targets POSIX systems; these package names
also identify the dependency for future Windows support.

### RPC dependencies and offline builds

`MRTK_WITH_RPC` defaults to `ON` and enables the WebSocket endpoint of
`mrtk run`. CMake finds the system CivetWeb package with
`find_package(civetweb CONFIG)` and requires version 1.16 or newer with WebSocket
support. Install it before configuring; CivetWeb is not downloaded by the
build. If its CMake package is outside the usual search paths, add its
prefix to `CMAKE_PREFIX_PATH`.

CMake first looks for a yyjson 0.11 or newer CMake package. If it cannot find
one, FetchContent downloads the hash-verified yyjson 0.13.0 release archive
and builds it with tests, fuzzers, miscellaneous tools and documentation
disabled. This requires network access during configuration, including on
Ubuntu 24.04, where yyjson is not provided by the default system packages,
and on Debian 13, whose package (0.10) is too old.
No third-party source is stored in this repository.

For an offline build, install a suitable yyjson CMake package, or provide an
already extracted yyjson 0.13.0 source tree:

```bash
cmake --preset default -DFETCHCONTENT_SOURCE_DIR_YYJSON=/absolute/path/to/yyjson-0.13.0
```

To omit both RPC dependencies, configure with:

```bash
cmake --preset default -DMRTK_WITH_RPC=OFF
```

That build rejects `--rpc-port`, `--rpc-bind`, `--rpc-origin`,
`--rpc-no-auth`, `--no-console` and `--exit-on-stdin-eof` with an error.
RPC transport tests are registered only when `MRTK_WITH_RPC=ON`.

## Build from Source

```bash
git clone https://github.com/h-shiono/MRTKLIB.git
cd MRTKLIB

# Configure
cmake --preset default

# Build
cmake --build build
```

The build produces a single binary `build/mrtk` with all subcommands.

## Verify Installation

```bash
# Check version
build/mrtk --version

# Show available subcommands
build/mrtk --help
```

Expected output:

```
mrtk: MRTKLIB unified CLI (MRTKLIB ver.0.6.2)

Usage: mrtk [COMMAND] [OPTIONS]

Core Commands:
  run         Run real-time positioning pipeline (rtkrcv)
  post        Run post-processing positioning (rnx2rtkp)

Data & Streaming:
  relay       Relay and split data streams (str2str)
  convert     Convert receiver raw data to RINEX (convbin)

Format Translation:
  ssr2obs     Convert SSR corrections to pseudo-observations
  ssr2osr     Convert SSR corrections to OSR

Utilities:
  bias        Estimate receiver fractional biases
  dump        Dump stream data to human-readable format (e.g., 'mrtk dump cssr')
```

## Run Tests

```bash
cd build
ctest --output-on-failure
```

!!! note "Real-time tests"
    Tests labeled `realtime` use file-stream replay (ranging from ~90 seconds to several minutes) and are excluded from CI.
    To run them locally:

    ```bash
    ctest --output-on-failure -L realtime
    ```

## Next Steps

- [First Run](first-run.md) — Run your first positioning solution
- [CLI Reference](../guide/cli.md) — Learn all `mrtk` subcommands
- [Configuration](../guide/configuration.md) — Set up TOML configuration files
