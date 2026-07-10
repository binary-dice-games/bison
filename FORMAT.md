# Bison Binary Wire Format

This document specifies the binary encoding produced by `bison::dynamic`
serialisation so that other libraries can implement compatible readers and
writers without depending on the C++ source.

---

## 1. Conventions

### 1.1 Byte order

All multi-byte integers are written in **big-endian** (network) byte order.
On little-endian hosts the reference implementation swaps bytes on every read
and write; on big-endian hosts the bytes are passed through unchanged.

### 1.2 Primitive widths

| C++ type   | Width | Notes |
|------------|------:|-------|
| `uint8_t`  | 1 B   | type tag, boolean payload |
| `uint32_t` | 4 B   | `hash_t`, `key_t`, `hash_t`-typed field payload |
| `int32_t`  | 4 B   | signed 32-bit integer field payload |
| `float`    | 4 B   | IEEE 754 single-precision |
| `size_t`   | 8 B   | element/byte count prefix (platform `sizeof(size_t)` on the **writing** host; the reference implementation is 64-bit) |

> **Note on `size_t` width.** The reference implementation writes `size_t` as
> 8 bytes on 64-bit hosts.  Portable reimplementations should treat all length
> prefixes as 8-byte big-endian unsigned integers.

### 1.3 Name hashing (FNV-1a)

String field names are not transmitted as text; they are transmitted as their
32-bit FNV-1a hash.  The same hash function is used to look them up at the
receiving end.

Algorithm (32-bit FNV-1a, MSB forced to 1):

```
offset_basis = 0x811c9dc5
prime        = 0x01000193

hash = offset_basis
for each byte b in the UTF-8 name:
    hash = (hash XOR b) * prime
hash = hash OR 0x80000000
```

The MSB is always set so that hashed names are distinguishable from
plain numeric indices (small non-negative integers that share the same
`key_t` map slot but have the MSB clear).

The following table lists all hash values used in the standard field and
class registry names referenced by the serialisation layer.

| String literal           | Hash (hex) | Used as |
|--------------------------|-----------|---------|
| `"__class"`              | see note  | `CLASS` reserved field key |
| `"__parent"`             | see note  | `PARENT` reserved field key |
| `"__namespace"`          | see note  | `NAMESPACE` reserved field key |

> Pre-computed hash values can be obtained at compile time with the C++ literal
> `"__class"_key`, etc.  An implementing library only needs to reproduce the
> FNV-1a formula above to derive them.

---

## 2. Field Encoding

A `field` is a discriminated union (variant).  Each serialised field begins
with a **1-byte type tag** followed by the type-specific payload.

### 2.1 Type tags

The tag value equals the zero-based index of the variant alternative in the
`field_base` declaration order.  The table is **stable**; changing the order
requires a protocol version bump.

| Tag | C++ type                | Payload description |
|-----|------------------------|---------------------|
| `0` | `monostate` (null)     | *(no payload)* |
| `1` | `hash_t`               | 4 bytes big-endian `uint32_t` |
| `2` | `key_t`                | 4 bytes big-endian `uint32_t` (same wire representation as `hash_t`) |
| `3` | `bool`                 | 1 byte: `0x00` = false, `0x01` = true |
| `4` | `int32_t`              | 4 bytes big-endian signed integer |
| `5` | `float`                | 4 bytes IEEE 754 single-precision, big-endian |
| `6` | `dynamic_ptr` (object) | 1-byte presence flag, then nested object (see §2.2) |
| `7` | `std::string`          | 8-byte big-endian length, then UTF-8 bytes |
| `8` | `vector<bool>`         | 8-byte big-endian count, then *count* × 1-byte elements |
| `9` | `vector<int32_t>`      | 8-byte big-endian count, then *count* × 4-byte big-endian `int32_t` |
| `10`| `vector<float>`        | 8-byte big-endian count, then *count* × 4-byte big-endian `float` |
| `11`| `vector<uint8_t>`      | 8-byte big-endian count, then *count* raw bytes |

### 2.2 Nested `dynamic_ptr` (tag 6)

```
[1 byte: presence flag]
```

- If the flag is `0x00`: the field holds a null object pointer.  No further
  bytes follow for this field.
- If the flag is `0x01`: the nested `dynamic` object follows immediately,
  encoded in **Standard Format** (see §3.1).

### 2.3 String and array length prefixes

All variable-length payloads (strings and vectors) are prefixed with an 8-byte
big-endian unsigned integer giving the number of **elements** (not bytes) that
follow.

---

## 3. `dynamic` Object Encoding

There are two encoding modes for a `dynamic` object.

### 3.1 Standard Format (self-describing)

Used by `dynamic::serialize` / `dynamic::deserialize`.  Requires no prior
knowledge of any class schema.

```
[8 bytes BE: field_count  (number of key-value pairs to follow)]
for each field (field_count times):
    [4 bytes BE: key  (hash_t / key_t of the field name)]
    [field encoding (§2)]
```

Fields are written in the iteration order of the internal `std::map<key_t, …>`,
which is ascending numeric order of the hash value.  Deserialisers should
accept fields in any order.

The reserved fields `__class`, `__parent`, and `__namespace` are included in
the output whenever they are present on the instance.

### 3.2 Schema-Driven Compact Format

Used by `dynamic::serializeWithSchema` / `dynamic::deserializeWithSchema`.
More compact because field keys are omitted; the decoder must have the same
class prototypes registered in its registry.

```
[4 bytes BE: namespace_id  (key_t; 0x00000000 = global namespace)]
[4 bytes BE: class_id      (key_t hash of the class name)]
for each field declared in the class prototype chain (prototype order):
    [field encoding (§2)]  ← value only, no key
```

Prototype chain traversal:

1. The writer looks up `class_id` in the namespace `namespace_id` of the
   class registry.
2. It iterates all fields of that prototype in `std::map` ascending order.
3. It then follows the `__parent` link to the parent class prototype
   (if any) and repeats step 2, continuing until the chain is exhausted
   (`__parent` is `0` / not found).
4. For each prototype field the writer emits either the instance's own value
   (if present) or the prototype's default value.

The decoder performs the same chain traversal and reads exactly one `field`
encoding (§2) per prototype field, assigning it to the corresponding key.

> **Important:** the reader and writer must have identical class registries
> (same namespace, same class name, same field set, same insertion order).
> Any difference silently corrupts the decoded object.

When `namespace_id` is present but the corresponding namespace is not in the
reader's registry, the decoder returns an empty `dynamic` object.

---

## 4. RMI Envelope Format

The RMI layer wraps serialised `dynamic` payloads inside an **envelope**
that is itself serialised using Schema-Driven Compact Format (§3.2) with the
pre-registered class `"__envelope"`.

### 4.1 Envelope schema (`"__envelope"`)

| Field name         | String literal    | Wire tag | Type        | Meaning |
|--------------------|-------------------|----------|-------------|---------|
| `__version`        | `"__version"`     | 4 (`int32_t`) | Protocol version; current value = **1** |
| `__kind`           | `"__kind"`        | 2 (`key_t`)   | Message kind token (see §4.2) |
| `__op`             | `"__op"`          | 2 (`key_t`)   | Operation token (see §4.3) |
| `__requestId`      | `"__requestId"`   | 2 (`key_t`)   | Correlation ID; echoed back in the response |
| `__objectId`       | `"__objectId"`    | 2 (`key_t`)   | Target server-side object identifier |
| `__group`          | `"__group"`       | 2 (`key_t`)   | Group newly-created objects are filed under (`0` = none); also the target group for `"destroyGroup"` (see §4.3, `context::current_group`) |
| `__payload`        | `"__payload"`     | 11 (`vector<uint8_t>`) | Serialised payload `dynamic` (see §4.4) |
| `__error`          | `"__error"`       | 11 (`vector<uint8_t>`) | Serialised error `dynamic` (see §4.5) |
| `__withSchema`     | `"__withSchema"`  | 3 (`bool`)    | Whether the payload was encoded in schema-driven mode |
| `__oneway`         | `"__oneway"`      | 3 (`bool`)    | If true the sender does not expect a response |

The envelope is encoded in Schema-Driven Compact Format (§3.2), so on the wire
its fields are **not** in the declaration order shown above — §3.2's decoder
visits prototype fields in ascending hash order. As of this field set, that
order is:

```
[4 bytes BE: namespace_id = 0x00000000  (global)]
[4 bytes BE: class_id     = hash("__envelope")]
[field: __withSchema — bool]
[field: __objectId   — key_t]
[field: __op         — key_t]
[field: __kind       — key_t]
[field: __group      — key_t]
[field: __requestId  — key_t]
[field: __payload    — vector<uint8_t>]
[field: __oneway     — bool]
[field: __version    — int32_t]
[field: __error      — vector<uint8_t>]
```

This order is an implementation detail of each field name's hash, not a
stable contract — it changes if a field is renamed, and a newly added field
lands wherever its hash happens to sort. Do not hand-construct envelope bytes
assuming a fixed order; always go through `envelope::encode()`/`decode()` (or
an equivalent implementation that performs the same prototype-chain
traversal), which computes the order at runtime from the registered schema.

### 4.2 Message kind tokens

| Token string | Meaning |
|--------------|---------|
| `"request"`  | Client-to-server request |
| `"response"` | Server-to-client reply |
| `"event"`    | Server-initiated asynchronous notification |

### 4.3 Operation tokens

| Token string   | Meaning |
|----------------|---------|
| `"connect"`    | Initial handshake |
| `"describe"`   | Request class schema information |
| `"instantiate"`| Create a server-side object |
| `"clear"`      | Reset all fields of a server-side object |
| `"set"`        | Merge fields into a server-side object |
| `"get"`        | Retrieve fields from a server-side object |
| `"call"`       | Invoke a method on a server-side object |
| `"destroy"`    | Destroy a server-side object |
| `"destroyGroup"` | Destroy every object filed under `__group` (see §4.1, `context::current_group`/`put_object()`); a no-op if the group is empty or unknown |
| `"disconnect"` | Graceful connection teardown |
| `"event"`      | Event payload dispatch (used with kind `"event"`) |

### 4.4 Payload bytes (`__payload`)

The bytes stored in `__payload` are either:

- **Standard Format** (§3.1) when `__withSchema` is `false`.
- **Schema-Driven Compact Format** (§3.2) when `__withSchema` is `true`.

### 4.5 Error bytes (`__error`)

The bytes stored in `__error` are always encoded in Schema-Driven Compact
Format (§3.2) using the pre-registered class `"__error"`.

**Error schema (`"__error"`) — field order:**

| Field name    | String literal  | Wire tag | Type       | Meaning |
|---------------|-----------------|----------|------------|---------|
| `__code`      | `"__code"`      | 2 (`key_t`)    | Canonical error code token |
| `__message`   | `"__message"`   | 7 (`string`)   | Human-readable message |
| `__details`   | `"__details"`   | 6 (`dynamic_ptr`) | Optional structured details object |

When there is no error the bytes encode an `"__error"` object with all fields
at their default (zero / empty) values.

### 4.6 Canonical error code tokens

| Token string          | Meaning |
|-----------------------|---------|
| `"INVALID_REQUEST"`   | Request could not be parsed or validated |
| `"UNSUPPORTED_VERSION"` | Protocol version mismatch |
| `"UNKNOWN_OPERATION"` | Unrecognised operation token |
| `"CLASS_NOT_FOUND"`   | Requested class is not in the registry |
| `"OBJECT_NOT_FOUND"`  | Requested object ID does not exist in session |
| `"ACCESS_DENIED"`     | Request denied by policy |
| `"VALIDATION_ERROR"`  | Payload failed semantic validation |
| `"INTERNAL_ERROR"`    | Unhandled server-side exception |
| `"TIMEOUT"`           | Request timed out |
| `"TRANSPORT_ERROR"`   | Underlying transport channel failure |

---

## 5. Transport Framing

The envelope byte buffer produced in §4 is transmitted over the transport layer
wrapped in a lightweight length-prefix frame.

### 5.1 TCP / socket transport

```
[4 bytes BE uint32_t: payload_length]
[payload_length bytes: envelope bytes]
```

The length field is written using `htonl` / read using `ntohl`, i.e. standard
network byte order (big-endian), and represents the number of bytes in the
payload that immediately follows.

### 5.2 Term transport (OSC-99 client→server, `BISON<...>` server→client)

The term transport (`term_client_transport` / `term_server_transport`,
`src/rmi/transport/term_transport.hpp`) is relayed through a real
pseudo-console (ConPTY on Windows, a pty on Linux/MSYS2, via
`src/term/terminal.hpp`). Unlike every other transport in this document, it
uses **two different wire formats**, one per direction, because a
pseudo-console's two pipes are not symmetric:

- **Client→server** (client's stdout → pseudo-console's *output* pipe →
  server's read fd): terminal emulators and ConPTY are built to shepherd
  OSC (Operating System Command) escape sequences through as atomic, opaque
  units — even ones they don't recognize — rather than mangling arbitrary
  text embedded in the stream, which is what makes the literal `BISON<...>`
  text from §5.2 unreliable here. This direction uses OSC-99 framing
  (below).
- **Server→client** (server's write fd → pseudo-console's *input* pipe →
  client's stdin): this side is not a passthrough channel. On Windows,
  ConPTY runs everything written here through its VT **input** state
  machine to synthesize keyboard `INPUT_RECORD`s for the child process; it
  only recognizes a small fixed table of real input sequences (arrow/
  function keys, mouse reports, bracketed paste, focus events) and has no
  "pass unrecognized bytes through verbatim" contract, so an OSC-99
  sequence written here is silently absorbed by the input parser and never
  reaches the child as literal bytes. This direction instead reuses §5.2's
  literal `BISON<base64(frame)>` text verbatim, un-chunked: plain ASCII
  with no `ESC` byte anywhere, so it can never be mistaken for an input
  control sequence, and the pseudo-console delivers it to the child exactly
  like typed characters. (This exact asymmetry, and the same fix, is
  independently documented in this repo's `src/term/ANALYSIS.md` §2, which
  uses plain literal text for its own parent→child command channel for the
  identical reason.)

This split is a protocol-level property of the term transport, not a
platform branch: a POSIX pty passes either wire format through unmolested
in both directions, so the same code runs unchanged on Linux/MSYS2 — it
just only matters operationally on native Windows/ConPTY.

#### 5.2.1 Client→server: OSC-99 framing

Each chunk has the form:

```
ESC ] 99 ; <seq> ; <total> ; base64(chunk bytes) BEL
```

i.e. `\x1b]99;<seq>;<total>;<base64(chunk)>\x07`, where:

- `<seq>` is the 0-based index of this chunk within the envelope, in
  decimal ASCII.
- `<total>` is the total chunk count for the envelope, in decimal ASCII.
- The whole sequence, including the `\x1b]99;`/`;`/`;`/`\x07` framing
  bytes, is capped at `kMaxOscSequenceBytes` (4096) bytes. An envelope
  small enough to fit in one chunk is sent as a single `seq=0;total=1`
  sequence — chunking only activates for larger envelopes, so the common
  case carries no extra round trips.
- A multi-chunk envelope's chunks are sent back-to-back, in order, with no
  other OSC-99 traffic interleaved between them; the receiver reassembles
  by concatenating decoded chunk bytes as `seq` runs from `0` to
  `total - 1`, then delivers the full envelope once `seq == total - 1`
  completes.
- Receiving `seq == 0` always starts a fresh reassembly, discarding (with a
  logged warning) any prior incomplete one. Receiving a chunk whose `seq`
  or `total` doesn't match the in-progress reassembly's expectations also
  discards the in-progress reassembly (logged) and drops that chunk — this
  bounds memory on a stalled/interrupted transfer instead of accumulating
  forever.
- Exactly like `BISON<...>`, every byte that is not part of a recognized
  OSC-99 sequence (the shell's actual output, unrelated OSC sequences such
  as window-title updates, etc.) is forwarded verbatim to a passthrough
  callback, keeping the terminal session fully interactive.

#### 5.2.2 Server→client: `BISON<...>` marker framing

Identical to §5.2's framing:

```
BISON< base64(frame bytes) >
```

One marker per whole envelope — no chunking, no `kMaxOscSequenceBytes`-style
size cap, since that cap exists only to bound a single escape sequence's
length, and this format is not an escape sequence. As with OSC-99, every
byte that is not part of a recognized marker is forwarded verbatim to a
passthrough callback.

#### 5.2.3 Connect / disconnect

Term transport has no transport-level handshake. `"connect"`/`"disconnect"`
(§4.3) are ordinary envelopes sent through the normal client/server RMI
round trip (`client::connect()`/`client::disconnect()`), carried over the
same §5.2.1/§5.2.2 framing as every other operation — a `"connect"`
request is one more OSC-99 sequence client→server, its response one more
`BISON<...>` marker server→client. The transport's `open()` performs no
I/O; it only arms a timeout so that a client with no live peer on the
other end fails the connect call instead of hanging (see
`term_client_transport::is_connected()`'s doc comment).

---

## 6. Annotated Byte-Level Examples

### 6.1 Standard-format `dynamic` with two scalar fields

Object: `{ "x": int32(3), "y": int32(7) }`  
(assuming `hash("x") = 0xABCD1234` and `hash("y") = 0xABCD5678` for illustration)

```
00 00 00 00 00 00 00 02   ← field_count = 2 (8-byte BE)
AB CD 12 34              ← key = hash("x") (4-byte BE uint32_t)
04                       ← tag = 4 (int32_t)
00 00 00 03              ← value = 3 (4-byte BE int32_t)
AB CD 56 78              ← key = hash("y")
04                       ← tag = 4 (int32_t)
00 00 00 07              ← value = 7
```

### 6.2 Standard-format field — null string

```
07                       ← tag = 7 (std::string)
00 00 00 00 00 00 00 00  ← length = 0 (8-byte BE)
                         ← (no bytes follow)
```

### 6.3 Standard-format field — nested null object (tag 6)

```
06                       ← tag = 6 (dynamic_ptr)
00                       ← presence flag = 0 (null pointer)
```

### 6.4 Standard-format field — nested non-null object (tag 6)

```
06                       ← tag = 6 (dynamic_ptr)
01                       ← presence flag = 1 (object follows)
<standard-format dynamic encoding>
```

### 6.5 Schema-driven compact `dynamic`

Object of class `"Point"` (registered in global namespace) with fields
`x = 5`, `y = 9` in prototype order:

```
00 00 00 00              ← namespace_id = 0 (global)
<hash("Point") BE u32>   ← class_id
04                       ← tag = 4 (int32_t) — field x
00 00 00 05              ← value = 5
04                       ← tag = 4 (int32_t) — field y
00 00 00 09              ← value = 9
```

---

## 7. Implementation Notes

1. **Field type stability.** Once a field is written with a given tag, the same
   tag must be used for all subsequent writes of that field.  The reference
   implementation enforces this at the C++ type level.

2. **Attributes are never serialised.** Per-field metadata attached via the
   `attribute` API is purely in-memory and has no wire representation.

3. **Methods are never serialised.** The `methods_` map on a `dynamic` object
   is not included in any binary encoding.

4. **Numeric vs. named keys.** Keys with the MSB clear (values `0x00000000`–
   `0x7FFFFFFF`) are treated as plain numeric indices (array-style access).
   Keys with the MSB set are FNV-1a name hashes.  Both share the same `key_t`
   map; callers should avoid creating accidental collisions between the two
   spaces.

5. **Schema consistency.** Schema-driven (compact) deserialisation silently
   returns an empty `dynamic` if the namespace or class is not registered.
   It does not perform any length-based validation; a schema mismatch will
   corrupt the decoded object without raising an error.

6. **Protocol version.** The current protocol version constant is **1**.
   Implementations should reject (or at least warn on) envelopes whose
   `__version` field does not match the expected value.
