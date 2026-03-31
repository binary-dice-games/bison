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
- Additional accepts after the first active connection return nullopt.
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
- ACK: optional control acknowledgement
- ERR: framing-level errors (not RMI protocol errors)

### 5.2 Why DCS

DCS blocks are intended as control strings and are typically not rendered.
This allows protocol frames and user-visible logs to share stdout.

### 5.3 Fallback mode

Some terminals or relay chains may strip/alter DCS sequences. The transport
supports a line-oriented fallback mode:

@@BISON_RMI@@<body>\n
Where body is the same key-value payload as above.

Mode negotiation:

1. Server emits HELLO in DCS mode and line mode during startup window.
2. Client accepts either and replies in detected mode.
3. Session mode is fixed after first successful control frame exchange.

## 6. Message Chunking

To tolerate terminal line/packet limits, each binary frame may be split into
chunks before base64 wrapping.

- id identifies one logical frame.
- seq is zero-based chunk index.
- total is chunk count.
- b64 is chunk payload.

Reassembly rules:

- Chunks must have consistent id and total.
- Duplicate seq is rejected.
- Reassembly completes when all seq values [0, total-1] arrive.
- Chunks received after timeout are dropped.

Default constraints:

- max_decoded_frame_bytes = 8 MiB
- max_chunk_b64_bytes = 2048
- max_inflight_messages = 32
- reassembly_timeout_ms = 5000

## 7. Runtime Architecture

### 7.1 Reader thread

Each endpoint owns a parser thread that continuously reads stdin bytes,
extracts framed control envelopes, and pushes decoded DATA bytes into an
internal queue.

Non-protocol text handling:

- Text that is not part of control frames is ignored by transport parser.
- Optional debug mode may mirror unmatched text to stderr.

### 7.2 Writer

send(frame) performs:

1. Split frame into chunks.
2. Base64 encode each chunk.
3. Emit wrapped control frame to stdout.
4. Flush stdout after each chunk.

### 7.3 Blocking receive

receive(out, timeout) waits on queue condition variable and returns:

- true when a complete decoded frame is available.
- false on timeout or closed state.

## 8. Session Lifecycle

### 8.1 Startup

Server:

1. start() initializes parser and writer state.
2. Emits HELLO control frame.
3. accept() returns one stdio_server_connection once running.

Client:

1. open() initializes parser and writer state.
2. Waits for HELLO up to handshake_timeout_ms.
3. Marks transport open.

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
- send() protected by a mutex for frame atomicity.
- Shared state protected by mutex + condition_variable.
- close flag uses atomic bool.

Locking goals:

- Keep lock scope narrow.
- Never hold parser lock while writing stdout.

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

- Parser errors on one malformed frame do not crash transport.
- Fatal stream errors transition transport to closed state.

## 11. Parameters

Initial dynamic params accepted by open/start:

- mode: auto | dcs | line
- handshake_timeout_ms: int32
- receive_timeout_ms: int32 (default used by receive)
- reassembly_timeout_ms: int32
- max_frame_bytes: int32
- max_chunk_b64_bytes: int32
- mirror_plaintext_to_stderr: bool

Transport should read both plain and internal names, matching current style:

- mode and __mode
- handshake_timeout_ms and __handshake_timeout_ms
- etc.

## 12. Platform Notes

### 12.1 Windows

- Use binary mode for stdin/stdout when possible.
- Avoid CRLF transformations in protocol bytes by keeping binary at transport
  edges and base64 for payload.

### 12.2 POSIX

- Works on pipes and PTY links.
- If terminal driver rewrites control bytes, fallback line mode remains usable.

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

## 16. Open Questions For Review

- Should server emit HELLO continuously until first client DATA frame, or once?
- Should END be transport-level only, or also require RMI disconnect first?
- Should plaintext passthrough be ignored, mirrored, or callback-driven?
- Should line fallback always be enabled, or opt-in for stricter channels?
