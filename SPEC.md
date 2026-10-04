# BH/1 — HTTP semantics over binary frames

**Rajveer Bishnoi** — Roll No. 10404

**Status:** v1, course project. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.
All multi-byte integers are **unsigned, big-endian** (network byte order).

## 1. Model

A client opens **one TCP connection** and sends a series of requests over it. Each request gets
exactly one response. A request and its response share a **stream ID**. In v1 the server handles
requests strictly in the order received and sends each response in full before the next; a client
MAY pipeline (send several requests before reading), but the responses still arrive in order.
The connection stays open until either side closes it **between** messages.

## 2. Connection preface

The client's first 8 bytes on a new connection MUST be:

```
42 48 2F 31 0D 0A 1A 0A        "BH/1" CR LF ^Z LF
```

A server that receives anything else MUST close the connection without replying (the peer is not
speaking BH/1, so it could not parse a reply). The `CR LF ^Z LF` tail is borrowed from PNG: a
text-mode transfer or a plain-HTTP client hitting the port fails on byte 0–7 instead of
producing garbage frames. The server sends no preface. The `1` changes only for a revision
that is *not* backward compatible; compatible extensions use new frame types (§4).

## 3. Frame header (8 bytes, fixed)

Every message after the preface is a sequence of **frames**: an 8-byte header followed by
exactly `Length` bytes of payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-----------------------------------------------+---------------+
|                 Length (24)                   |   Type (8)    |
+---------------+-----------------------------------------------+
|   Flags (8)   |                 Stream ID (24)                |
+---------------+-----------------------------------------------+
|                     Payload (Length bytes) ...
```

| Field | Width | Meaning |
|---|---|---|
| Length | 24 bits | Payload size in bytes, **not** counting the header. 0 – 16 777 215. |
| Type | 8 bits | What the payload is (§4). |
| Flags | 8 bits | Per-type booleans. Undefined bits MUST be sent as 0 and MUST be ignored on receipt. |
| Stream ID | 24 bits | Which request/response this frame belongs to. 0 is reserved for connection-level frames. |

**Why these widths.** *Length first*, so a receiver can always skip a frame with nothing but the
first three bytes — that is what makes §4's skip rule implementable. *24-bit length:* a 16 MiB frame
is larger than any header block anyone should send, and bodies are split across DATA frames
anyway, so a 32-bit length buys only the ability to force a peer to buffer 4 GiB. *8-bit type:*
256 types is room for decades of extensions; we define two. *8-bit flags:* we use one bit and
keep seven for v2 (e.g. compression, priority). *24-bit stream ID:* v1 never has two streams
active, so the ID is a correlation number and a hook for v2 multiplexing; 16 M requests per
connection is far more than any connection will live for, and 24 bits lets the header fit in
**exactly 8 bytes** — one 64-bit load, and offsets that stay simple to compute by hand.

**Why HTTP/2 chose 24/8/8/31.** Same length and type/flags reasoning, plus a *small default*
max frame (16 KiB) so one large body cannot hog a multiplexed connection. Its stream ID is 31
bits because HTTP/2 streams are **never reused** on a long-lived connection (client-odd,
server-even — 2³⁰ each), and the 32nd bit was reserved, partly so implementations in languages
without unsigned 32-bit ints (Java) never see a negative ID. That gives a 9-byte header; we trade
HTTP/2's huge ID space for an aligned 8-byte one because v1 has no concurrency.

## 4. Frame types, flags and the skip rule

| Type | Name | Stream | Payload |
|---|---|---|---|
| `0x00` | DATA | ≠ 0 | Body bytes, opaque. Length MAY be 0. |
| `0x01` | HEADERS | ≠ 0 | A header block (§5). Starts a request or response. |
| `0x02`–`0xFF` | — | any | Reserved. `0xF0`–`0xFF` are for experiments / "greasing". |

Flag `0x01` **END_STREAM**: this frame is the last of its message. All other bits are reserved.

> **A receiver that meets a frame type it does not know MUST read and discard exactly `Length`
> payload bytes and continue as if the frame had never been sent.** It MUST NOT treat this as an
> error, whatever the frame's flags or stream ID.

This is the extension mechanism. A v1.x peer can add, say, a `PING` (type `0x02`) frame, and a
v1 peer will skip it. To stop implementations ossifying around "only types 0 and 1 ever appear",
servers MAY **grease**: send frames of a random type in `0xF0`–`0xFF` at any frame boundary
(reference server: `bserve -g`). Unknown flag bits are ignored for the same reason.

## 5. Header blocks

A HEADERS payload is a sequence of fields, packed back-to-back until the payload ends.
Each field is:

```
+---------+--------------------------------+---------------+---------------+
| Index 8 | [NameLen 8 | Name …] if Index=0 |  ValueLen 16  |   Value …     |
+---------+--------------------------------+---------------+---------------+
```

* `Index` 1–10 names the field from the **static table**. `Index = 0` means a *literal name*
  follows: a 1-byte length (1–255) then the name. `Index` 11–255 is reserved → malformed.
* `ValueLen` (0–65 535) then the value bytes. Values MUST NOT contain `0x00`, CR or LF (so
  every BH/1 message can be translated to HTTP/1.1).
* Literal names MUST be lowercase visible ASCII (`0x21`–`0x7E`, no `A`–`Z`, no `:`). A sender
  MUST use the index when the name is in the table.

| # | Name | # | Name |
|---|---|---|---|
| 1 | `:method` | 6 | `accept` |
| 2 | `:path` | 7 | `content-type` |
| 3 | `:status` | 8 | `content-length` |
| 4 | `host` | 9 | `server` |
| 5 | `user-agent` | 10 | `date` |

These are exactly the ten names our client and server send, so normal traffic contains no
literal names at all. This is HPACK's first two mechanisms (static table + length-prefixed
literals) without the dynamic table and Huffman coding — those save bytes but add state that
must stay in sync on both ends; v1 chooses simplicity. A literal name costs 2 + n bytes,
an indexed one 1 byte; every value costs 2 + n bytes.

Fields whose names start with `:` are **pseudo-fields**. They MUST come before all regular
fields, MUST NOT repeat, and MUST be sent by index.

## 6. Messages

**Request.** One HEADERS frame on a new nonzero stream ID with END_STREAM set (v1 requests have
no body). It MUST contain `:method` (`GET` or `HEAD`) and `:path` (starts with `/`; may contain
`%XX` escapes and a `?query`, which the server ignores). It SHOULD contain `host`. Clients SHOULD
use stream IDs 1, 2, 3, … in order.

**Response.** On the **same stream ID** as the request: one HEADERS frame whose **first** field is
`:status` (three ASCII digits, e.g. `"200"`), followed by zero or more DATA frames. The last frame
of the response carries END_STREAM — on the HEADERS frame if there is no body (HEAD, empty file),
otherwise on the final DATA frame (which MAY be empty). Servers SHOULD send `content-length`;
clients SHOULD check it. A receiver MUST NOT assume DATA frames have any particular size.
A server MUST NOT send a second HEADERS frame on a stream (no trailers in v1).

**Mapping paths.** The server percent-decodes `:path`, maps it under its document root, serves
`index.html` for directories, and MUST NOT serve anything that resolves outside the root
(`..`, symlinks) — such requests get `404`.

## 7. Errors

| Situation | Who | Action |
|---|---|---|
| Wrong preface | server | Close. No reply. |
| Malformed header block (bad index, truncated field, bad name, NUL/CR/LF, missing or duplicate or misordered pseudo-field, `:path` not starting `/`), request without END_STREAM, request on stream 0, header block > 64 KiB | server | Reply **400** on that stream; **keep the connection**. |
| Method other than GET/HEAD | server | **405**, with `allow: GET, HEAD`. |
| No such file / outside root | server | **404**. |
| Unknown frame type | both | Skip (§4). Never an error. |
| DATA frame received by a server | server | Ignore. |
| EOF in the middle of a frame or before END_STREAM | both | The message is incomplete; close. |
| Sender can't finish a body it started (e.g. file shrank) | server | Close the connection — status already sent cannot be retracted. |

Because `Length` is always valid, a malformed *payload* never desynchronises the stream: the
receiver has already consumed exactly `Length` bytes, so it can reply 400 and read the next frame.
Error responses carry a short `text/plain` body. A client exits non-zero on any 4xx/5xx.

## 8. Limits and room for v2

Receivers MUST accept DATA frames of any length; they MAY refuse HEADERS larger than 64 KiB (→ 400,
having read and discarded the payload). Senders SHOULD keep DATA frames ≤ 16 KiB. Room left on
purpose: types `0x02`–`0xEF`, seven flag bits, static indices 11–255, stream 0 for connection
frames, and stream IDs that already allow multiplexing once a v2 lifts the one-at-a-time rule.
