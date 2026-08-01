# Orama

Orama is a C++23 quantitative trading bot. It trains an XGBoost multiclass classifier
(buy/hold/sell) on OHLCV+technical-indicator bars and runs a live trading loop against
Interactive Brokers' TWS API, with an optional ImGui/ImPlot dashboard for monitoring
targets, positions, and account stats in real time.

## Prerequisites

Everything except a C++23 toolchain and (optionally) IBKR's own SDK is fetched and
built automatically by CMake (`FetchContent`) — there's no need to hunt down XGBoost,
TA-Lib, protobuf, or the GUI stack through a package manager first.

- **macOS**: Xcode command line tools or Homebrew LLVM, plus `brew install libomp`
  (Apple Clang doesn't ship OpenMP support itself).
- **Linux**: `clang` (or `gcc`) and the usual build tooling (`cmake`, `ninja`,
  `build-essential`/`base-devel`).
- **Windows**: Visual Studio 2022 (or a recent standalone MSVC toolset) with the
  "Desktop development with C++" workload. Configure/build from an **x64 Native Tools
  Command Prompt** (or a shell that's run `vcvarsall.bat`) so `cl.exe`/`link.exe`/`ninja`
  are on `PATH`.
- CMake ≥ 3.30 (required by the vendored TA-Lib build on Windows).

## Building

CMake presets are provided for all three platforms:

```sh
# macOS
cmake --preset release && cmake --build --preset release
cmake --preset debug   && cmake --build --preset debug     # ASan+UBSan

# Linux (also the VPS/Docker deploy target)
cmake --preset linux-release && cmake --build --preset linux-release
cmake --preset linux-debug   && cmake --build --preset linux-debug   # ASan+UBSan

# Windows (from an x64 Native Tools Command Prompt)
cmake --preset windows-release && cmake --build --preset windows-release
cmake --preset windows-debug   && cmake --build --preset windows-debug   # ASan only — MSVC has no UBSan
```

The first configure will take a while: it clones and builds XGBoost, TA-Lib, protobuf,
and (if the GUI is enabled) GLFW/ImGui/ImPlot from source.

### Disabling the GUI

The live dashboard (`ORAMA_USE_GUI`, default **ON**) pulls in OpenGL/GLFW/ImGui/ImPlot,
which need a display (X11/Wayland/Win32 window server) to actually run — not something a
headless Docker container or bare VPS has. Turn it off for those deployments:

```sh
cmake --preset linux-release -DORAMA_USE_GUI=OFF
```

### IBKR TWS API setup (required to build the trading engine)

The live trading engine talks to Interactive Brokers through their official C++ TWS
API. That SDK isn't included in this repo — IBKR's license terms around
redistribution aren't something this project can resolve on your behalf, so you need to
add it yourself:

1. Download the TWS API C++ client from IBKR's API page (see "TWS API" under
   interactivebrokers.com's API software downloads, or interactivebrokers.github.io).
2. Copy the contents of the SDK's `source/cppclient/client/` directory into
   `core/twsapi/` at the repo root.
3. Copy the platform-appropriate decimal-math library from the same SDK download
   (bundled per-OS — a `.a` on macOS/Linux, a `.lib` on Windows) into
   `core/twsapi/libbid/`.

Without this step, `core/broker/broker.hpp` (included from the manager and user
modules — i.e. the whole trading engine) won't find `EClientSocket.h` and friends, and
the build will fail. This is inherent to not being able to ship IBKR's SDK here, on any
platform; it's not a bug in the CMake setup.

## Known limitations

- The broker/IBKR dependency is baked directly into `manager`/`user`, not behind an
  abstraction — so there's currently no way to build `orama` without the TWS API SDK
  present. Decoupling that would be a real architecture change, not a build-portability
  fix.
- Windows support hasn't been verified end-to-end on real hardware (this project is
  developed on macOS and deployed on Linux) — the MSVC branches in `CMakeLists.txt`
  compile the same flags as Clang/GNU where a direct equivalent exists, but please file
  an issue if something doesn't configure/build cleanly there.

## License

See [LICENSE](LICENSE). This applies to Orama's own source only — it does not cover
the IBKR TWS API (not included in this repo; obtained separately under IBKR's own
terms) or any other third-party dependency fetched during the build.
