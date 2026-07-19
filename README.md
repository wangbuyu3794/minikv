# MiniKV

MiniKV is an educational key-value database written in C17. Its primary target
platform is Linux, with development and Linux validation performed through WSL
when the host is Windows.

The M0 milestone intentionally contains only a minimal, buildable, and testable
project skeleton. It provides a small core library, a command-line entry point,
and smoke tests. It does not yet implement a database server.

## Requirements

- CMake 3.16 or newer
- A C17-capable C compiler
- A CMake generator supported by the local toolchain

MiniKV has no third-party runtime dependencies.

## Layout

```text
apps/             Executable entry points
include/minikv/   Public headers
src/              Reusable implementation
tests/            Lightweight C tests
```

## Build and test on Windows

Use a dedicated build tree so Windows artifacts are not mixed with WSL
artifacts:

```powershell
cmake -S . -B build/windows
cmake --build build/windows --config Debug
ctest --test-dir build/windows -C Debug --output-on-failure
```

For a single-configuration generator such as Ninja, select the configuration
during configure instead:

```powershell
cmake -S . -B build/windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/windows
ctest --test-dir build/windows --output-on-failure
```

## Build and test on WSL/Linux

Run these commands from the repository root inside WSL:

```sh
cmake -S . -B build/wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build/wsl
ctest --test-dir build/wsl --output-on-failure
./build/wsl/minikv-server --help
```

## M0 behavior

`minikv-server --help` and `minikv-server -h` print usage information. Running
the program without arguments reports that the M0 skeleton cannot start a
server and exits with a nonzero status.

Networking, RESP2, the key-value data structure, AOF persistence, and
configuration files are deliberately outside the M0 scope.
