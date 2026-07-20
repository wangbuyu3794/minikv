# MiniKV

MiniKV is an educational key-value server written in C17. The project aims to
build a Redis-compatible protocol and database features incrementally, with
Linux as its primary runtime platform.

The current M2 milestone contains:

- the M1 Linux nonblocking TCP server skeleton;
- a platform-neutral incremental RESP2 parser.

The parser is not connected to the network server. MiniKV does not execute or
dispatch commands, implement `PING`, `ECHO`, `GET`, or `SET`, store data, or
send protocol responses. It has no TTL, persistence, or authentication and is
not a usable Redis server.

## M1 network server

On Linux, the server currently provides:

- an IPv4 TCP listener;
- nonblocking and close-on-exec file descriptors;
- level-triggered epoll;
- accept queue draining;
- receiving and discarding client data;
- cleanup when clients disconnect;
- SIGINT and SIGTERM handling through signalfd;
- a successful exit status after a normal signal-driven stop.

The default address is `127.0.0.1`, the default port is `6379`, and port 0
requests an ephemeral port. Bind addresses must be numeric IPv4 addresses.

The server drains accepted client data and discards it. It does not parse that
data, execute commands, or send a response. The M2 parser is not connected to
client sockets yet.

## M2 RESP2 parser

The independent parser supports these RESP2 value types:

- Simple String (`+`);
- Simple Error (`-`);
- Integer (`:`);
- Bulk String (`$`);
- Null Bulk String (`$-1`);
- Array (`*`);
- Null Array (`*-1`).

It handles empty values, nested arrays, null elements inside arrays,
binary-safe bulk payloads, arbitrary input fragmentation, and multiple
top-level values in one input buffer. Bulk strings may contain NUL, CR, LF,
and embedded CRLF bytes.

The parser is incremental and does not retain caller input pointers. It copies
bytes that must outlive a feed call and returns one complete top-level value at
a time. The consumed-byte count stops exactly after that value, leaving any
following value for the next call.

Truncated but otherwise valid input remains in the `NEED_MORE` state. M2 has no
EOF or finalize operation. After a parse error, callers can reset and reuse the
parser. Test builds also provide deterministic allocation-failure hooks for
memory ownership and cleanup testing.

## Internal API and ownership

Parser declarations live in `src/resp2_internal.h`. This is an internal project
and test interface: it is not installed below `include/minikv/`, and no stable
public ABI is promised.

The parser owns incomplete values. Ownership of a completed value tree
transfers to the caller, independently of the input buffer.
`minikv_resp2_value_destroy()` recursively releases a returned tree. Parser
reset and destroy operations release any incomplete state still owned by the
parser.

## RESP2 resource limits

The parser uses these defaults:

| Limit | Default |
|---|---:|
| Line length | 65,536 bytes |
| Single bulk payload | 16,777,216 bytes |
| Direct array elements | 65,536 |
| Nesting depth | 128 |
| Value nodes | 1,000,000 |
| Total payload per top-level value | 67,108,864 bytes |

Limits are copied into each parser, and tests can supply smaller custom limits.
Length, growth, payload-total, and allocation-size arithmetic is checked for
overflow.

## Requirements

- CMake 3.16 or newer
- a C17-capable C compiler
- a CMake generator supported by the local toolchain

MiniKV has no third-party runtime dependencies.

## Platform support

The `minikv_resp2` static library and its test are platform-neutral. They use
standard C17 and do not depend on the server runtime.

Linux uses the real socket, level-triggered epoll, and signalfd server backend.
Other platforms build the command-line program with an unsupported network
backend; help remains available, but attempting to start the service reports
that networking is unsupported and returns an error.

A Windows build validates the platform-neutral parser, CLI, and core behavior.
It does not compile or validate the Linux network backend.

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
- `minikv.resp2.parser`
- `minikv.server.linux`
- `minikv.server.integration`

The platform-neutral parser test covers valid values, binary payloads,
fragmented input, consumed-byte boundaries, invalid syntax, resource limits,
lifecycle behavior, and deterministic allocation failures.

The lifecycle test directly exercises the runtime interface, file-descriptor
flags, accept draining, data draining, and connection cleanup. The integration
test uses fork and exec to start the real `minikv-server` executable and checks
ephemeral-port startup, a real TCP connection, SIGTERM shutdown, and CLI exit
statuses.

The existing GitHub Actions workflow configures testing and will run all
registered Linux CTests for GCC and Clang in Debug and Release configurations.
This M2 branch has not yet received formal remote Linux CI validation.

## Build and test on Windows

The following example uses MinGW Makefiles and a dedicated Windows build tree:

```powershell
cmake -S . -B build/windows -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=ON

cmake --build build/windows --parallel

ctest --test-dir build/windows `
  --output-on-failure `
  --no-tests=error
```

Windows registers these platform-neutral tests:

- `minikv.smoke`
- `minikv.server.help`
- `minikv.server.short-help`
- `minikv.resp2.parser`

The parser test has been run locally with MinGW in Debug and Release
configurations. Linux-specific server tests remain registered only on Linux.

With `BUILD_TESTING=OFF`, CMake still builds the `minikv_resp2` library but does
not build or register `minikv_resp2_parser_test`.

## Repository layout

```text
.github/workflows/ci.yml
apps/minikv_server.c
include/minikv/version.h
src/version.c
src/resp2_internal.h
src/resp2_parser.c
src/server_internal.h
src/server_linux.c
src/server_unsupported.c
tests/resp2_parser_test.c
tests/smoke_test.c
tests/server_linux_test.c
tests/server_integration_test.c
CMakeLists.txt
README.md
```

Executable entry points live in `apps/`, reusable implementation lives in
`src/`, public headers live under `include/minikv/`, and tests live in `tests/`.
`src/server_internal.h` is an internal runtime and test interface, not a stable
public API. `src/resp2_internal.h` has the same internal status.

`minikv_resp2` is a platform-neutral static library.
`minikv_resp2_parser_test` links only to that library, not to
`minikv_server_runtime`.

## Current limitations

- the RESP2 parser is not connected to the server runtime;
- no RESP encoder;
- no inline commands;
- no RESP3;
- no command parsing, dispatch, or execution;
- no network protocol responses;
- no database storage;
- no TTL;
- no persistence;
- no authentication;
- numeric IPv4 server bind addresses only;
- no parser EOF or finalize operation;
- the real network backend currently targets Linux.

Planned directions include connecting RESP2 parsing to client connections,
adding command dispatch and basic commands, and then adding an in-memory
database and TTL. No release dates are promised.
