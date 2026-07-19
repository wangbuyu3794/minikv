# MiniKV

MiniKV is an educational key-value server written in C17. The project aims to
build a Redis-compatible protocol and database features incrementally, with
Linux as its primary runtime platform.

The current M1 milestone provides only the network server skeleton. It does not
implement RESP, `SET`, `GET`, TTL, persistence, or a functional key-value
database, and it cannot be used as a Redis replacement.

## M1 status

On Linux, the server currently provides:

- an IPv4 TCP listener;
- nonblocking and close-on-exec file descriptors;
- level-triggered epoll;
- accept queue draining;
- receiving and discarding client data;
- cleanup when clients disconnect;
- SIGINT and SIGTERM handling through signalfd;
- a successful exit status after a normal signal-driven stop.

Client data is not parsed, and the server sends no protocol response. No Redis
commands are implemented.

## Requirements

- CMake 3.16 or newer
- a C17-capable C compiler
- a CMake generator supported by the local toolchain

MiniKV has no third-party runtime dependencies.

## Platform support

Linux uses the real socket, epoll, and signalfd backend. Other platforms can
build the command-line program and the unsupported backend; help remains
available, but attempting to start the server normally reports that the network
service is unsupported and returns an error.

A Windows build validates only the platform-neutral CLI and core behavior. It
does not compile or validate the Linux network backend.

## Command-line interface

The current defaults are:

- bind address: `127.0.0.1`
- port: `6379`

`--bind` accepts only a numeric IPv4 address. `--port` accepts values from 0
through 65535; port 0 asks the operating system to allocate an ephemeral port.
Hostnames, IPv6, and configuration files are not supported.

Examples:

```sh
minikv-server --help
minikv-server -h
minikv-server
minikv-server --bind 127.0.0.1 --port 6380
minikv-server --bind 127.0.0.1 --port 0
```

After a successful start, the server prints the actual listening address and
port:

```text
MiniKV listening on 127.0.0.1:<port>
```

The current exit-status behavior is:

- `0`: help was displayed, or the server stopped normally;
- `1`: a runtime or backend error, such as an invalid bind address or an
  unsupported platform;
- `2`: a command-line syntax error, such as an unknown, duplicate, missing, or
  out-of-range argument.

## Build and test on Linux

Run these commands from the repository root:

```sh
cmake -S . -B build/linux \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON

cmake --build build/linux --parallel

ctest --test-dir build/linux \
  --output-on-failure \
  --no-tests=error
```

Linux registers these CTest tests:

- `minikv.smoke`
- `minikv.server.help`
- `minikv.server.short-help`
- `minikv.server.linux`
- `minikv.server.integration`

The lifecycle test directly exercises the runtime interface, file-descriptor
flags, accept draining, data draining, and connection cleanup. The integration
test uses fork and exec to start the real `minikv-server` executable and checks
ephemeral-port startup, a real TCP connection, SIGTERM shutdown, and CLI exit
statuses.

## Build and test on Windows

The following example uses MinGW Makefiles and a dedicated Windows build tree:

```powershell
cmake -S . -B build/windows -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=ON

cmake --build build/windows --parallel

ctest --test-dir build/windows --output-on-failure
```

Windows registers only the platform-neutral tests:

- `minikv.smoke`
- `minikv.server.help`
- `minikv.server.short-help`

## Repository layout

```text
.github/workflows/ci.yml
apps/minikv_server.c
include/minikv/version.h
src/version.c
src/server_internal.h
src/server_linux.c
src/server_unsupported.c
tests/smoke_test.c
tests/server_linux_test.c
tests/server_integration_test.c
CMakeLists.txt
README.md
```

Executable entry points live in `apps/`, reusable implementation lives in
`src/`, public headers live under `include/minikv/`, and tests live in `tests/`.
`src/server_internal.h` is an internal runtime and test interface, not a stable
public API.

## Current limitations

- no RESP parser;
- no command execution;
- no database storage;
- no TTL;
- no persistence;
- no authentication;
- only numeric IPv4 bind addresses;
- the Linux network backend is the primary supported runtime.

Planned directions include a RESP parser, basic commands, an in-memory
database, TTL, and persistence. No release dates are promised.
