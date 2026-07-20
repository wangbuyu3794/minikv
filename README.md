# MiniKV

MiniKV is an educational key-value server written in C17. The project aims to
build a Redis-compatible protocol and database features incrementally, with
Linux as its primary runtime platform.

The current M3 milestone contains:

- the M1 Linux nonblocking TCP server skeleton;
- the M2 platform-neutral incremental RESP2 parser;
- a platform-neutral RESP2 response encoder and command dispatcher;
- a Linux RESP2 request-response loop with `PING` and `ECHO`;
- fragmented-input and pipelined-request handling;
- buffered nonblocking output with per-client backpressure isolation.

MiniKV remains an educational, incremental project rather than a Redis
replacement. It does not implement a key-value database, `GET`, `SET`, TTL,
persistence, authentication, RESP3, or full Redis compatibility.

## M1 network server

M1 introduced the Linux server runtime with:

- an IPv4 TCP listener;
- nonblocking and close-on-exec file descriptors;
- level-triggered epoll;
- accept queue draining;
- nonblocking client input;
- cleanup when clients disconnect;
- SIGINT and SIGTERM handling through signalfd;
- a successful exit status after a normal signal-driven stop.

The default address is `127.0.0.1`, the default port is `6379`, and port 0
requests an ephemeral port. Bind addresses must be numeric IPv4 addresses.

M3 now connects RESP2 parsing and command execution to these client sockets.
The network and signal-handling foundation remains the M1 design.

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

The response encoder and command dispatcher also use internal declarations.
The encoder does not allocate memory: callers provide the destination buffer.
Command replies either reference static data or temporarily borrow a Bulk
String payload from the parsed request. The caller must encode a borrowed reply
before destroying that request.

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

## M3 RESP2 command loop

On Linux, each accepted connection owns an independent incremental parser and
an output buffer. The server reads nonblocking socket input, parses complete
RESP2 requests, dispatches commands, encodes replies, and queues those replies
for nonblocking delivery.

Commands are case-insensitive for ASCII letters and must use a RESP2 Array of
one or more non-null Bulk Strings. The first element is the non-empty command
name. Arguments are also non-null Bulk Strings. Other complete RESP2 values,
nested command elements, and null Bulk String elements are protocol errors.

The implemented commands are:

- `PING` returns the Simple String `PONG`;
- `PING <message>` returns `<message>` as a binary-safe Bulk String;
- `ECHO <message>` returns `<message>` as a binary-safe Bulk String;
- wrong command arity returns a command-specific Simple Error;
- an otherwise valid but unknown command returns
  `-ERR unknown command\r\n`.

`PING` and `ECHO` messages may contain NUL, CR, LF, and CRLF bytes. Input may
arrive in arbitrary fragments, and multiple requests may be pipelined in one
byte stream. Replies remain in request order.

Client output is buffered per connection. The level-triggered event loop
enables `EPOLLOUT` only while bytes are pending, handles partial writes,
`EINTR`, and `EAGAIN`, and keeps other clients responsive when one client
delays reading. Pending output is limited to 33,554,432 bytes (32 MiB) per
connection; a reply that would exceed the limit closes only that client
without sending a partial reply.

For malformed RESP2 or an invalid command-frame shape, the server attempts to
send the exact response `-ERR Protocol error\r\n` after any already queued
complete replies, then closes the connection. Allocation failures and internal
state errors close the affected connection without being reported as client
protocol errors.

When a peer half-closes after a complete request, queued replies are sent
before the server closes the connection. If EOF arrives with an incomplete
request, the server closes the connection without fabricating a protocol-error
response.

## Requirements

- CMake 3.16 or newer
- a C17-capable C compiler
- a CMake generator supported by the local toolchain

MiniKV has no third-party runtime dependencies.

## Platform support

The `minikv_resp2` and `minikv_command` static libraries and their unit tests
are platform-neutral. They use standard C17 and do not depend on the Linux
server runtime.

Linux uses the real socket, level-triggered epoll, and signalfd server backend.
Other platforms build the command-line program with an unsupported network
backend; help remains available, but attempting to start the service reports
that networking is unsupported and returns an error.

A Windows build validates the platform-neutral parser, encoder, command
dispatcher, CLI help, and core behavior. It does not compile or validate the
Linux network backend or the RESP2 network command loop.

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

## Build and run on Linux

Run these commands from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build

./build/minikv-server --help
./build/minikv-server --bind 127.0.0.1 --port 6379
```

## Test on Linux

Use a separate build tree when enabling tests:

```sh
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Linux registers these CTest tests:

- `minikv.smoke`
- `minikv.server.help`
- `minikv.server.short-help`
- `minikv.resp2.parser`
- `minikv.resp2.encoder`
- `minikv.command`
- `minikv.server.linux`
- `minikv.server.integration`

The platform-neutral tests cover parsing, encoding, command dispatch, binary
payloads, invalid input, resource limits, overflow checks, and ownership. The
Linux lifecycle test deterministically covers forced `EAGAIN`, short writes,
pipeline ordering, output-limit boundaries, and connection cleanup. The
integration test starts the real `minikv-server` process and covers fragmented
requests, pipelines, multiple clients, protocol errors, half-close and EOF
behavior, large `ECHO` replies, backpressure isolation, connection reuse, and
signal-driven shutdown.

The existing GitHub Actions workflow configures testing and will run all
registered Linux CTests for GCC and Clang in Debug and Release configurations.

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
- `minikv.resp2.encoder`
- `minikv.command`

Linux-specific lifecycle and integration tests remain registered only on
Linux. Non-Linux builds use the unsupported server backend and do not validate
the real network command loop.

With `BUILD_TESTING=OFF`, CMake still builds the production libraries and
`minikv-server`, including `minikv_resp2` and `minikv_command`, but does not
build or register test executables or test-only hooks.

## Repository layout

```text
.github/workflows/ci.yml
apps/minikv_server.c
include/minikv/version.h
src/command.c
src/command_internal.h
src/resp2_internal.h
src/resp2_encoder.c
src/resp2_parser.c
src/server_internal.h
src/server_linux.c
src/server_unsupported.c
src/version.c
tests/command_test.c
tests/resp2_encoder_test.c
tests/resp2_parser_test.c
tests/server_integration_test.c
tests/server_linux_test.c
tests/smoke_test.c
CMakeLists.txt
README.md
```

Executable entry points live in `apps/`, reusable implementation lives in
`src/`, public headers live under `include/minikv/`, and tests live in `tests/`.
`src/server_internal.h`, `src/resp2_internal.h`, and
`src/command_internal.h` are internal runtime and test interfaces, not stable
public APIs.

`minikv_resp2` and `minikv_command` are platform-neutral static libraries. The
Linux `minikv_server_runtime` links them to provide the network command loop;
the unsupported backend does not.

## Current limitations

- no inline commands;
- no RESP3;
- only `PING` and `ECHO` commands;
- no `GET`, `SET`, or other database commands;
- no database storage;
- no TTL;
- no persistence;
- no authentication;
- no multithreading;
- no full Redis compatibility;
- numeric IPv4 server bind addresses only;
- no parser EOF or finalize operation;
- the real network backend currently targets Linux.

Future milestones may add database functionality, but no release dates or
feature commitments are promised.
