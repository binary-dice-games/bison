# RMI Stdio Transport Design

## 1. Overview

This document defines a new RMI transport that tunnels envelope frames over
stdin and stdout. The target use cases are terminal-mediated links such as
ssh, adb shell, and generic console pipes where a TCP socket is unavailable or
undesired.

The transport must coexist with regular text output printed by applications,
while still carrying binary RMI envelopes reliably.

## 2. Goals

- Reuse the existing client/server transport contract without changing RMI core.
- Carry envelope bytes over stdin/stdout using ASCII-safe framing.
- Keep regular terminal text usable during an active session.
- Support graceful remote session termination from the client side.
- Be robust against partial reads, buffering differences, and noisy stdout.

## 3. Non-Goals

- Multi-client multiplexing in one server process.
- Encryption, authentication, or compression at transport layer.
- TTY emulation or shell job control features.

## 4. Public API Shape

The new transport follows existing transport naming and signatures.

### 4.1 Client

- stdio_client_transport::open(dynamic&& params)
- stdio_client_transport::send(buffer frame)
- stdio_client_transport::receive(buffer& frame, milliseconds timeout)
- stdio_client_transport::shutdown()

### 4.2 Server listener

- stdio_server_transport::start(dynamic params)
- stdio_server_transport::accept(milliseconds timeout)
- stdio_server_transport::stop()

### 4.3 Server connection

- stdio_server_connection::send(buffer frame)
- stdio_server_connection::receive(buffer& frame, milliseconds timeout)
- stdio_server_connection::close()
- stdio_server_connection::is_closed() const

Behavior notes:

- Accept returns a single connection once started.
- Additional accepts after the first connection block on a condition variable
  until stop() is called or the timeout elapses; they do not busy-wait.
- Stop closes the active connection and causes blocked receive calls to wake.

## 5. Framing Strategy

Raw envelope bytes are encoded as base64 and wrapped in terminal control
sequences so terminals usually do not render protocol payloads.

### 5.1 Control envelope format

Preferred hidden frame carrier:

- Start: ESC P (DCS)
- End: ESC \\ (ST)

Body format (ASCII key-value):

BISON_RMI/1;type=DATA;id=<u64>;seq=<u32>;total=<u32>;b64=<base64>

Other type values:

- HELLO: readiness signal from server transport after startup
- END: graceful session termination request

Note: ACK and ERR frame types are reserved but not currently used.

### 5.2 Why DCS

DCS blocks are intended as control strings and are typically not rendered.
This allows protocol frames and user-visible logs to share stdout.

### 5.3 Fallback mode

Some terminals or relay chains may strip/alter DCS sequences. The transport
supports a line-oriented fallback mode:

@@BISON_RMI@@<body>\n
Where body is the same key-value payload as above.

Mode negotiation:

1. Server emits HELLO in DCS mode and line mode during startup (in auto_detect
   mode, both are emitted; otherwise only the configured mode is used).
2. Client opens() and waits for any HELLO; the first recognized HELLO fixes the
   session framing mode.
3. Mode is fixed for the lifetime of the session after the first HELLO exchange.

## 6. Message Chunking

To tolerate terminal line/packet limits, each binary frame may be split into
chunks before base64 wrapping.

- id identifies one logical frame.
- seq is zero-based chunk index.
- total is chunk count.
- b64 is chunk payload.

Reassembly rules:

- Chunks must have consistent id and total.
- Duplicate seq is ignored.
- Reassembly completes when all seq values [0, total-1] arrive.
- Chunks received after timeout are dropped.

Default constraints:

- max_frame_bytes = 8 MiB
- max_chunk_bytes = 1536 bytes (chunk payload before base64 expansion)
- reassembly_timeout_ms = 5000

Note: there is no limit on the number of concurrent in-flight messages.

## 7. Runtime Architecture

### 7.1 Reader thread

Each endpoint owns a parser thread that continuously reads stdin bytes,
extracts framed control envelopes, and pushes decoded DATA bytes into an
internal queue.

The reader thread cannot be safely interrupted during a blocking syscall read,
so shutdown() and stop() detach the thread rather than joining it. This makes
each transport instance single-use: it cannot be reopened after shutdown.

Non-protocol text handling:

- Text that is not part of control frames is ignored by transport parser.
- Optional debug mode may mirror unmatched text to stderr.

### 7.2 Writer

send(frame) performs:

1. Split frame into chunks.
2. Base64 encode each chunk.
3. Emit wrapped control frame to stdout.
4. Flush stdout after each chunk.

Empty frames are sent as a single DATA message with total=1 and an empty b64
field, so the receiver can deliver a zero-byte payload.

### 7.3 Blocking receive

receive(out, timeout) waits on queue condition variable and returns:

- true when a complete decoded frame is available.
- false on timeout or closed state.

## 8. Session Lifecycle

### 8.1 Startup

Server:

1. start() initializes parser and writer state.
2. Emits HELLO control frame (both carriers in auto_detect mode).
3. accept() returns one stdio_server_connection once running.

Client:

1. open() initializes parser and writer state.
2. Waits for HELLO up to handshake_timeout_ms.
3. Locks the session mode from the first recognized HELLO carrier.
4. Marks transport open.

### 8.2 Normal traffic

- Client and server exchange DATA frames carrying encoded envelope bytes.
- RMI one-way and request/response semantics remain unchanged.

### 8.3 Graceful shutdown

Client shutdown sequence:

1. RMI disconnect operation (normal protocol)
2. Transport END control frame
3. Local close

Server behavior on END:

- Close connection
- Unblock receive
- Allow server app to return control to terminal

## 9. Concurrency Model

- One reader thread per endpoint.
- send() protected by write_mtx for frame atomicity.
- Inbox and reassembly state protected by read_mtx + read_cv.
- close and stop flags use atomic bools.
- negotiated_mode uses atomic int so it can be set from the reader thread and
  read from the write thread without a lock.

Locking goals:

- Keep lock scope narrow.
- Never hold read_mtx while writing stdout.
- Release read_mtx before notifying read_cv (wake first caller, not all).

## 10. Error Handling

Transport-level failures map to runtime_error with clear context.

Examples:

- malformed control frame
- invalid base64 payload
- chunk metadata mismatch
- oversized frame
- handshake timeout
- stream closed

Behavior policy:

- Parser errors on one malformed frame do not crash the transport.
- Fatal stream errors transition transport to closed state.

## 11. Parameters

Parameters accepted by open/start (plain and __-prefixed forms both work):

- mode: auto | dcs | line (default: dcs for client, auto for server)
- handshake_timeout_ms: int32 (default: 5000)
- reassembly_timeout_ms: int32 (default: 5000)
- max_frame_bytes: int32 (default: 8388608, minimum 1024)
- max_chunk_b64_bytes: int32 (default: 1536, minimum 64)
- mirror_plaintext_to_stderr: bool (default: false)

Note: receive_timeout_ms is not stored in shared_state; the caller passes a
timeout directly to each receive() call.

## 12. Platform Notes

### 12.1 Windows

- Use binary mode for stdin/stdout when possible.
- Avoid CRLF transformations in protocol bytes by keeping binary at transport
  edges and base64 for payload.

### 12.2 POSIX

- Works on pipes and PTY links.
- If terminal driver rewrites control bytes, fallback line mode remains usable.
- On Linux, the server transport switches the controlling terminal to raw,
  no-echo mode when reading from std::cin on a TTY and restores it on stop().

## 13. Security and Safety Considerations

- Do not execute terminal control content from peer.
- Enforce strict parser grammar and size limits.
- Drop unknown keys in control body.
- Avoid exposing decoded binary payload in logs unless debug-enabled.

## 14. Testing Plan

### 14.1 Unit tests (parser/framer)

- Encode/decode round trip for DATA frames.
- DCS and line mode parsing.
- Partial stream input across random boundaries.
- Chunk reassembly and timeout expiration.
- Oversize and malformed frame rejection.

### 14.2 Transport tests

- Client-to-server send/receive parity with existing transport tests.
- Server-to-client send/receive.
- accept timeout behavior before start and after stop.
- END closes both sides cleanly.
- Mixed plaintext output does not break framing.

### 14.3 Integration tests

- Spawn server example under pipe bridge and run client operations.
- Validate that non-protocol stdout remains visible.

## 15. Rollout Plan

1. Add stdio transport classes and framer utility.
2. Add parser/framer unit tests.
3. Add transport behavior tests alongside socket/memory tests.
4. Add stdio client/server examples.
5. Document usage patterns for ssh and adb workflows.

## 16. Design Decisions

This section records design questions that arose during implementation and
the choices made.

**Should the server emit HELLO continuously until the first client DATA frame,
or only once at startup?**

Once at startup. In auto_detect mode, HELLO is emitted in both DCS and line
format during start() so both carriers are covered. Repeated emission would
consume bandwidth and complicate the state machine.

**Should END be transport-level only, or also require RMI disconnect first?**

Transport-level only. The client sends END after completing the RMI-level
disconnect, but the transport does not enforce ordering. Receiving END
immediately closes the connection from the transport's perspective.

**Should plaintext passthrough be ignored, mirrored, or callback-driven?**

Mirrored to stderr when mirror_plaintext_to_stderr is true (default off).
Callback-driven mirroring adds API surface without a clear use case; the
stderr option covers diagnostics and keeps the API simple.

**Should line fallback always be enabled, or opt-in for stricter channels?**

The reader loop always recognises both DCS and line frames simultaneously, so
either carrier works regardless of the configured send mode. This costs nothing
and avoids requiring the user to know the channel type in advance.
