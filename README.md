# BH/1 — HTTP semantics over binary frames

[![build-and-test](https://github.com/RAJVEER42/bh1-binary-http/actions/workflows/ci.yml/badge.svg)](https://github.com/RAJVEER42/bh1-binary-http/actions/workflows/ci.yml)

**Rajveer Bishnoi** — Roll No. 10404 · Network Architecture course project

BH/1 carries HTTP's semantics — methods, paths, status codes, header fields, bodies — over a
**binary framing layer** instead of CRLF-delimited text. This repository contains the
specification, two interoperating C11 programs that implement it (`bserve`, a static file
server, and `bcurl`, a client), and an independent conformance suite written in Python.

The exercise is the design, not the plumbing: choose the frame header fields and their widths
and justify them, replace repeated header names with small integers, and make the format
extensible — a receiver must be able to skip a frame type it has never heard of.

| # | Deliverable | Where |
|---|---|---|
| 1 | The specification | [`SPEC.md`](SPEC.md) |
| 2 | The implementation | [`src/`](src/) → `bserve` (server), `bcurl` (client) |
| 3 | Annotated hexdump | [`HEXDUMP.md`](HEXDUMP.md) |

## The protocol in one screen

A client opens **one** TCP connection and opens it with an 8-byte preface:

```
42 48 2F 31 0D 0A 1A 0A        "BH/1" CR LF ^Z LF
```

The `CR LF ^Z LF` tail is borrowed from PNG: a text-mode transfer or a stray HTTP/1.1 client
fails on the very first bytes rather than producing plausible-looking garbage.

Everything after it is **frames** — a fixed 8-byte header, then exactly `Length` bytes:

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

Two types are defined — `0x00` DATA and `0x01` HEADERS — and one flag, `0x01` END_STREAM,
marking the last frame of a message. A request is a single HEADERS frame; a response is a
HEADERS frame followed by zero or more DATA frames, on the same stream ID.

Header fields drop the repeated names in favour of an index into a 10-entry static table, so
ordinary traffic transmits no field names at all:

| # | Name | # | Name | # | Name |
|---|---|---|---|---|---|
| 1 | `:method` | 5 | `user-agent` | 9 | `server` |
| 2 | `:path` | 6 | `accept` | 10 | `date` |
| 3 | `:status` | 7 | `content-type` | | |
| 4 | `host` | 8 | `content-length` | | |

Index `0` escapes to a literal name. An indexed name costs 1 byte against 2 + n for a literal.

**The skip rule.** Because `Length` sits first in the header, a receiver can discard a frame it
does not understand using only the first three bytes it read. Unknown frame types MUST be
skipped and unknown flag bits MUST be ignored — never treated as errors. That single rule is
the whole extension mechanism, and `bserve -g` exists to police it (see below).

[`SPEC.md`](SPEC.md) has the full normative text, including the rationale for each field width
and a comparison with HTTP/2's 9-byte header.

## Build and run

No dependencies beyond a C11 compiler and, for the tests, Python 3.

```sh
make                                    # builds ./bserve and ./bcurl
./bserve ./www 9000                     # serve ./www on port 9000
./bcurl -v localhost:9000/index.html    # fetch it, hexdumping every frame
```

### `bserve [-v] [-g] ROOT PORT`

Forks per connection and loops for keep-alive. Percent-decodes the path, maps it under `ROOT`,
serves `index.html` for directories, and refuses anything resolving outside the root.

| Flag | Effect |
|---|---|
| `-v` | Trace every frame to stderr as annotated hex |
| `-g` | **Grease**: emit a frame of a random reserved type before every response |

Greasing is the interesting one. Implementations ossify when an unused extension point is
never exercised, so `-g` deliberately sends frames no client can know, and any client that
fails to skip them breaks loudly during development rather than silently in v2.

### `bcurl [-v] [-I] [-i] URL...`

`URL` is `[bh://]host[:port][/path]`, defaulting to port 9000 and path `/`. Multiple URLs are
fetched in order over **one** connection and must share a host and port. Bodies go to stdout.

| Flag | Effect |
|---|---|
| `-v` | Hexdump every frame sent and received to stderr |
| `-I` | Send HEAD instead of GET, and print the response fields |
| `-i` | Print the response fields before the body |

| Exit | Meaning |
|---|---|
| `0` | Success |
| `2` | Usage error |
| `7` | Cannot connect |
| `8` | Protocol error or connection lost |
| `22` | Some response was 4xx or 5xx |

## A worked exchange

```
$ ./bcurl -v localhost:9000/hello.txt
> PREFACE (8 bytes)
>  0000  42 48 2f 31 0d 0a 1a 0a                           |BH/1....|
> HEADERS frame: type=0x01 len=54 flags=0x01 [END_STREAM] stream=1
>  0000  00 00 36 01 01 00 00 01  01 00 03 47 45 54 02 00  |..6........GET..|
...
< DATA frame: type=0x00 len=13 flags=0x01 [END_STREAM] stream=1
<  0000  00 00 0d 00 01 00 00 01  48 65 6c 6c 6f 2c 20 42  |........Hello, B|
<  0010  48 2f 31 21 0a                                    |H/1!.|
Hello, BH/1!
```

Seventy bytes up, 113 down. [`HEXDUMP.md`](HEXDUMP.md) walks every one of them, field by field.

## Tests

```sh
make test
```

[`tests/conformance.py`](tests/conformance.py) is a **second, independent implementation of
BH/1 in Python, written only from `SPEC.md`** and never consulting the C source. It plays the
client against `bserve` and a deliberately awkward server against `bcurl`, so each C program is
always tested against something other than its own partner — a shared misreading of the spec
cannot pass unnoticed.

The 21 cases cover directory indexes, multi-frame bodies, 404, 405, path traversal, HEAD,
keep-alive, pipelined ordering, literal header names, a rejected preface, unknown frame types
and unknown flag bits in both directions, truncation detection, the one-connection rule, and
nine separately-checked malformed header blocks — each of which must draw a `400` while the
connection stays open.

CI builds and runs the suite on Ubuntu and macOS with `-Wall -Wextra -Wpedantic`.

## Where to look

| If you are interested in | Read |
|---|---|
| Why the header is 24/8/8/24, and how that compares with HTTP/2 | [`SPEC.md`](SPEC.md) §3 |
| The skip rule and the extension story | [`SPEC.md`](SPEC.md) §4 |
| Header compression without the HPACK dynamic table | [`SPEC.md`](SPEC.md) §5 |
| Why a malformed payload never desynchronises the connection | [`SPEC.md`](SPEC.md) §7 |
| Bytes on the wire, annotated | [`HEXDUMP.md`](HEXDUMP.md) |

## Layout

```
SPEC.md            the normative specification
HEXDUMP.md         one complete exchange, byte by byte
src/proto.[ch]     framing, header-block codec, hexdump tracer (shared)
src/bserve.c       server: fork per connection, keep-alive loop
src/bcurl.c        client
tests/             independent Python conformance suite
www/               sample document root
.github/           CI: build and test on Ubuntu and macOS
```
