# bh1-binary-http

**Rajveer Bishnoi** — Roll No. 10404

**BH/1** is a small binary framing protocol for HTTP semantics, with a static file server
(`bserve`) and a client (`bcurl`) that speak it. It's a Network Architecture course project:
pick the frame header fields and widths, number the header names you send, and make sure a
receiver can skip frame types it doesn't know.


| Deliverable | File |
|---|---|
| 1. The spec | [`SPEC.md`](SPEC.md) |
| 2. The program | `src/` → `bserve` (server), `bcurl` (client) |
| 3. Annotated hexdump | [`HEXDUMP.md`](HEXDUMP.md) |

## Build & run

```sh
make                                   # builds ./bserve and ./bcurl (C11, no dependencies)
./bserve ./www 9000                    # serve ./www on port 9000
./bcurl -v localhost:9000/index.html   # fetch; -v hexdumps every frame to stderr
```

`bserve [-v] [-g] ROOT PORT`: `-v` traces frames, `-g` "greases" (sends an unknown frame
before each response, which catches clients that don't skip unknown types).

`bcurl [-v] [-I] [-i] URL...`: `-I` HEAD, `-i` print response fields. Several URLs are fetched
over **one** connection. Exit codes: `0` ok, `22` any 4xx/5xx, `7` can't connect, `8` protocol error.

### Example

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

Every byte of this exchange is explained in [`HEXDUMP.md`](HEXDUMP.md).

## Tests

```sh
make test
```

`tests/conformance.py` is a **separate implementation of BH/1 in Python, written only from
SPEC.md**. It acts as the client against `bserve` and as a (deliberately awkward) server against
`bcurl`, so each C program is tested against something other than its partner. It covers
keep-alive, pipelining, 400/404/405, path traversal, HEAD, large multi-frame bodies, unknown
frame types and flags (both directions), and the one-connection rule.

## Layout

```
src/proto.[ch]   framing, header-block codec, hexdump tracer (shared)
src/bserve.c     server: fork per connection, keep-alive loop
src/bcurl.c      client
tests/           independent Python conformance suite
www/             sample document root
.github/         CI: build + tests on Ubuntu and macOS
```
