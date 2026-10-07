# Annotated hexdump: one complete BH/1 request and response

**Rajveer Bishnoi** — Roll No. 10404

Captured with the real programs (the `date` value is whatever time it was):

```
$ printf 'Hello, BH/1!\n' > www/hello.txt
$ ./bserve ./www 9000 &
$ ./bcurl -v localhost:9000/hello.txt
```

Client → server: **70 bytes** (8 preface + 8 header + 54 payload).
Server → client: **113 bytes** (8 + 84 HEADERS, 8 + 13 DATA).

---

## 1. Client → server: connection preface (8 bytes)

```
0000  42 48 2f 31 0d 0a 1a 0a                           |BH/1....|
```

| Bytes | Value | Meaning |
|---|---|---|
| `42 48 2f 31` | `"BH/1"` | Protocol + major version |
| `0d 0a` | CR LF | Breaks if something rewrites line endings |
| `1a` | ^Z | Stops `type` on DOS/Windows; non-text byte |
| `0a` | LF | Breaks if LF is rewritten to CR LF |

## 2. Client → server: request HEADERS frame (8 + 54 bytes)

```
0000  00 00 36 01 01 00 00 01  01 00 03 47 45 54 02 00  |..6........GET..|
0010  0a 2f 68 65 6c 6c 6f 2e  74 78 74 04 00 0e 6c 6f  |./hello.txt...lo|
0020  63 61 6c 68 6f 73 74 3a  39 30 30 30 05 00 09 62  |calhost:9000...b|
0030  63 75 72 6c 2f 31 2e 30  06 00 03 2a 2f 2a        |curl/1.0...*/*|
```

**Frame header** (offsets 0x00–0x07)

| Off | Bytes | Field | Value |
|---|---|---|---|
| 00 | `00 00 36` | Length | 0x36 = **54** payload bytes |
| 03 | `01` | Type | **HEADERS** |
| 04 | `01` | Flags | **END_STREAM**: no request body follows |
| 05 | `00 00 01` | Stream ID | **1**: first request on this connection |

**Header block** (offsets 0x08–0x3D, 54 bytes, 5 fields, all indexed)

| Off | Bytes | Decoded |
|---|---|---|
| 08 | `01` | index 1 = `:method` |
| 09 | `00 03` | value length 3 |
| 0B | `47 45 54` | `GET` |
| 0E | `02` | index 2 = `:path` |
| 0F | `00 0a` | value length 10 |
| 11 | `2f 68 65 6c 6c 6f 2e 74 78 74` | `/hello.txt` |
| 1B | `04` | index 4 = `host` |
| 1C | `00 0e` | value length 14 |
| 1E | `6c 6f … 30 30` | `localhost:9000` |
| 2C | `05` | index 5 = `user-agent` |
| 2D | `00 09` | value length 9 |
| 2F | `62 63 … 2e 30` | `bcurl/1.0` |
| 38 | `06` | index 6 = `accept` |
| 39 | `00 03` | value length 3 |
| 3B | `2a 2f 2a` | `*/*` |

Check: 6 + 13 + 17 + 12 + 6 = **54** = Length. ✔ The payload ends exactly at the last value,
which is how the receiver knows the block is finished — there is no field count.

## 3. Server → client: response HEADERS frame (8 + 84 bytes)

```
0000  00 00 54 01 00 00 00 01  03 00 03 32 30 30 07 00  |..T........200..|
0010  19 74 65 78 74 2f 70 6c  61 69 6e 3b 20 63 68 61  |.text/plain; cha|
0020  72 73 65 74 3d 75 74 66  2d 38 08 00 02 31 33 09  |rset=utf-8...13.|
0030  00 0a 62 73 65 72 76 65  2f 31 2e 30 0a 00 1d 54  |..bserve/1.0...T|
0040  68 75 2c 20 30 31 20 4f  63 74 20 32 30 32 36 20  |hu, 01 Oct 2026 |
0050  31 37 3a 30 32 3a 35 31  20 47 4d 54              |17:02:51 GMT|
```

**Frame header**

| Off | Bytes | Field | Value |
|---|---|---|---|
| 00 | `00 00 54` | Length | 0x54 = **84** |
| 03 | `01` | Type | **HEADERS** |
| 04 | `00` | Flags | none: **a body follows** in DATA frames |
| 05 | `00 00 01` | Stream ID | **1**: answers request 1 |

**Header block**

| Off | Bytes | Decoded |
|---|---|---|
| 08 | `03` `00 03` `32 30 30` | `:status` = `200` (first field, as required) |
| 0E | `07` `00 19` `74 65 … 2d 38` | `content-type` = `text/plain; charset=utf-8` (25 bytes) |
| 2A | `08` `00 02` `31 33` | `content-length` = `13` |
| 2F | `09` `00 0a` `62 73 … 2e 30` | `server` = `bserve/1.0` |
| 3C | `0a` `00 1d` `54 68 … 4d 54` | `date` = `Thu, 01 Oct 2026 17:02:51 GMT` (29 bytes) |

Check: 6 + 28 + 5 + 13 + 32 = **84**. ✔

## 4. Server → client: DATA frame (8 + 13 bytes)

```
0000  00 00 0d 00 01 00 00 01  48 65 6c 6c 6f 2c 20 42  |........Hello, B|
0010  48 2f 31 21 0a                                    |H/1!.|
```

| Off | Bytes | Field | Value |
|---|---|---|---|
| 00 | `00 00 0d` | Length | **13** |
| 03 | `00` | Type | **DATA** |
| 04 | `01` | Flags | **END_STREAM**: response complete |
| 05 | `00 00 01` | Stream ID | **1** |
| 08 | `48 65 … 21 0a` | Body | `Hello, BH/1!\n`, 13 bytes = `content-length` ✔ |

END_STREAM on stream 1 tells `bcurl` the response is done. The connection is **still open**:
another request would go out as a HEADERS frame on stream 2. `bcurl` has nothing more to
fetch, so it closes the connection; `bserve` reads EOF at a frame boundary and closes too.

---

## Appendix: what a skipped unknown frame looks like

With `bserve -g`, the server sends a grease frame before every response. `bcurl -v` shows:

```
< UNKNOWN frame: type=0xfa len=7 flags=0x80 stream=1
<  0000  00 00 07 fa 80 00 00 01  73 6b 69 70 20 6d 65     |........skip me|
<  (unknown frame type 0xfa: skipped)
```

Length `00 00 07` is all the client needs: it reads 7 more bytes, throws them away, and
reads the next header. The type, the flag bit `0x80` and the payload are never interpreted.
