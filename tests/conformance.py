#!/usr/bin/env python3
"""BH/1 conformance tests.

This file is a second, independent implementation of BH/1 written from
SPEC.md only (it shares no code with the C programs). It plays client
against ./bserve and server against ./bcurl. If both C programs pass against
this, they interoperate through the spec, not through shared code.

    python3 tests/conformance.py
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFACE = b"BH/1\r\n\x1a\n"
DATA, HEADERS = 0x00, 0x01
END_STREAM = 0x01
STATIC = [None, ":method", ":path", ":status", "host", "user-agent", "accept",
          "content-type", "content-length", "server", "date"]


# ------------------------------------------------------------- wire format

def frame(ftype, flags, stream, payload=b""):
    n = len(payload)
    return struct.pack(">BHBBBH", n >> 16, n & 0xFFFF, ftype, flags,
                       stream >> 16, stream & 0xFFFF) + payload


def field(name, value):
    v = value.encode() if isinstance(value, str) else value
    if name in STATIC:
        head = bytes([STATIC.index(name)])
    else:
        head = b"\x00" + bytes([len(name)]) + name.encode()
    return head + struct.pack(">H", len(v)) + v


def block(*pairs):
    return b"".join(field(n, v) for n, v in pairs)


def parse_block(p):
    out, i = [], 0
    while i < len(p):
        idx = p[i]; i += 1
        if idx == 0:
            nl = p[i]; i += 1
            name = p[i:i + nl].decode(); i += nl
        else:
            assert 1 <= idx <= 10, f"bad static index {idx}"
            name = STATIC[idx]
        (vl,) = struct.unpack(">H", p[i:i + 2]); i += 2
        out.append((name, p[i:i + vl].decode())); i += vl
    return out


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"EOF after {len(buf)}/{n} bytes")
        buf += chunk
    return buf


def recv_frame(sock):
    h = recv_exact(sock, 8)
    length = h[0] << 16 | h[1] << 8 | h[2]
    stream = h[5] << 16 | h[6] << 8 | h[7]
    return h[3], h[4], stream, recv_exact(sock, length)


def read_response(sock, stream):
    """Returns (fields dict, body, number of DATA frames). Skips unknown types."""
    fields, body, ndata = None, b"", 0
    while True:
        t, flags, sid, payload = recv_frame(sock)
        if t not in (DATA, HEADERS):
            continue
        assert sid == stream, f"response on stream {sid}, expected {stream}"
        if t == HEADERS:
            pairs = parse_block(payload)
            assert pairs[0][0] == ":status", "first field must be :status"
            fields = dict(pairs)
        else:
            assert fields is not None, "DATA before HEADERS"
            body += payload
            ndata += 1
        if flags & END_STREAM:
            return fields, body, ndata


def get(path, method="GET", extra=()):
    return block((":method", method), (":path", path), ("host", "test"), *extra)


# ------------------------------------------------------------------ harness

PASSED, FAILED = [], []


def test(fn):
    try:
        fn()
        PASSED.append(fn.__name__)
        print(f"  ok    {fn.__name__}")
    except Exception as e:  # noqa: BLE001
        FAILED.append(fn.__name__)
        print(f"  FAIL  {fn.__name__}: {e!r}")
    return fn


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.settimeout(5)
    s.sendall(PREFACE)
    return s


# ============================================================ server tests

WWW = tempfile.mkdtemp(prefix="bh-www-")
with open(os.path.join(WWW, "index.html"), "wb") as f:
    f.write(b"<h1>hi</h1>\n")
BIG = os.urandom(100_000)
with open(os.path.join(WWW, "big.bin"), "wb") as f:
    f.write(BIG)
os.mkdir(os.path.join(WWW, "dir"))
with open(os.path.join(WWW, "dir", "index.html"), "wb") as f:
    f.write(b"dir index\n")
with open(os.path.join(os.path.dirname(WWW), "bh-secret.txt"), "wb") as f:
    f.write(b"secret\n")

SPORT = free_port()
server = subprocess.Popen([os.path.join(ROOT, "bserve"), WWW, str(SPORT)],
                          stderr=subprocess.DEVNULL)
wait_port(SPORT)
print(f"bserve on :{SPORT}")


@test
def get_index():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/index.html")))
    f, body, _ = read_response(s, 1)
    assert f[":status"] == "200" and body == b"<h1>hi</h1>\n"
    assert f["content-length"] == str(len(body))
    assert f["content-type"].startswith("text/html")


@test
def directory_maps_to_index():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/dir/")))
    f, body, _ = read_response(s, 1)
    assert f[":status"] == "200" and body == b"dir index\n"
    s.sendall(frame(HEADERS, END_STREAM, 2, get("/dir")))
    f, body, _ = read_response(s, 2)
    assert f[":status"] == "200" and body == b"dir index\n"


@test
def large_file_many_data_frames():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/big.bin")))
    f, body, ndata = read_response(s, 1)
    assert f[":status"] == "200" and body == BIG and ndata > 1


@test
def not_found_404():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/missing")))
    f, _, _ = read_response(s, 1)
    assert f[":status"] == "404"


@test
def traversal_stays_in_root():
    s = connect(SPORT)
    for i, p in enumerate(["/../bh-secret.txt", "/%2e%2e/bh-secret.txt",
                           "/dir/../../bh-secret.txt"], 1):
        s.sendall(frame(HEADERS, END_STREAM, i, get(p)))
        f, body, _ = read_response(s, i)
        assert f[":status"] == "404" and b"secret" not in body, p


@test
def head_has_no_body():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/big.bin", "HEAD")))
    t, flags, sid, payload = recv_frame(s)
    assert t == HEADERS and flags & END_STREAM
    assert dict(parse_block(payload))["content-length"] == str(len(BIG))


@test
def method_not_allowed_405():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1, get("/", "DELETE")))
    f, _, _ = read_response(s, 1)
    assert f[":status"] == "405"


@test
def unknown_frames_are_skipped():
    s = connect(SPORT)
    s.sendall(frame(0x7E, 0xFF, 0, b"\x00" * 300)          # connection-level
              + frame(0xFB, 0x00, 1, b"future extension")  # on the request's stream
              + frame(0x02, 0x00, 9, b"")                   # empty, unused type
              + frame(HEADERS, END_STREAM, 1, get("/index.html")))
    f, _, _ = read_response(s, 1)
    assert f[":status"] == "200"


@test
def unknown_flags_ignored():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM | 0xF0, 1, get("/index.html")))
    f, _, _ = read_response(s, 1)
    assert f[":status"] == "200"


@test
def literal_header_names_accepted():
    s = connect(SPORT)
    s.sendall(frame(HEADERS, END_STREAM, 1,
                    get("/index.html", extra=[("x-trace-id", "abc123")])))
    f, _, _ = read_response(s, 1)
    assert f[":status"] == "200"


MALFORMED = {
    "unknown static index": b"\xEE\x00\x01x",
    "truncated value": get("/index.html")[:-3],
    "missing :path": block((":method", "GET")),
    "uppercase literal name": get("/") + b"\x00\x03ABC\x00\x00",
    "literal pseudo name": get("/") + b"\x00\x04:foo\x00\x00",
    "pseudo after regular": block((":method", "GET"), ("host", "x"), (":path", "/")),
    "duplicate :path": get("/") + field(":path", "/x"),
    "relative path": get("index.html"),
    "LF in value": block((":method", "GET"), (":path", "/a\nb")),
}


@test
def malformed_gets_400_and_connection_survives():
    s = connect(SPORT)
    sid = 1
    for name, payload in MALFORMED.items():
        s.sendall(frame(HEADERS, END_STREAM, sid, payload))
        f, _, _ = read_response(s, sid)
        assert f[":status"] == "400", f"{name}: got {f[':status']}"
        sid += 1
    # no END_STREAM on the request
    s.sendall(frame(HEADERS, 0, sid, get("/index.html")))
    assert read_response(s, sid)[0][":status"] == "400"
    sid += 1
    # header block over 64 KiB
    s.sendall(frame(HEADERS, END_STREAM, sid,
                    get("/index.html", extra=[("x-pad", "a" * 65000)]) + b"\x00" * 1000))
    assert read_response(s, sid)[0][":status"] == "400"
    sid += 1
    # ... and the same connection still serves a good request
    s.sendall(frame(HEADERS, END_STREAM, sid, get("/index.html")))
    assert read_response(s, sid)[0][":status"] == "200"


@test
def many_requests_one_connection():
    s = connect(SPORT)
    for sid in range(1, 51):
        s.sendall(frame(HEADERS, END_STREAM, sid, get("/index.html")))
        assert read_response(s, sid)[0][":status"] == "200"


@test
def pipelined_requests_answered_in_order():
    s = connect(SPORT)
    s.sendall(b"".join(frame(HEADERS, END_STREAM, i, get(p))
                       for i, p in enumerate(["/index.html", "/nope", "/big.bin"], 1)))
    assert read_response(s, 1)[0][":status"] == "200"
    assert read_response(s, 2)[0][":status"] == "404"
    assert read_response(s, 3)[1] == BIG


@test
def bad_preface_closes():
    s = socket.create_connection(("127.0.0.1", SPORT), 5)
    s.settimeout(5)
    s.sendall(b"GET / HTTP/1.1\r\n\r\n")
    try:
        assert s.recv(100) == b""      # orderly close (FIN) ...
    except ConnectionResetError:
        pass                           # ... or RST, since we left bytes unread


# ============================================================ client tests

class FakeServer:
    """Minimal BH/1 server with knobs for misbehaving in legal (and illegal) ways."""

    def __init__(self, handler):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.port = self.sock.getsockname()[1]
        self.handler = handler
        self.connections = 0
        self.streams = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            c, _ = self.sock.accept()
            self.connections += 1
            threading.Thread(target=self._conn, args=(c,), daemon=True).start()

    def _conn(self, c):
        try:
            assert recv_exact(c, 8) == PREFACE
            while True:
                t, flags, sid, payload = recv_frame(c)
                if t != HEADERS:
                    continue
                self.streams.append(sid)
                if self.handler(c, sid, dict(parse_block(payload))) == "close":
                    break
        except EOFError:
            pass
        finally:
            c.close()


def resp(status, body=b"", ctype="text/plain"):
    return block((":status", str(status)), ("content-type", ctype),
                 ("content-length", str(len(body))))


def bcurl(*args):
    return subprocess.run([os.path.join(ROOT, "bcurl"), *args],
                          capture_output=True, timeout=10)


def grease_handler(c, sid, req):
    body = f"you asked for {req[':path']}\n".encode()
    out = frame(0xF7, 0xFF, 0, b"connection-level junk")
    out += frame(0x42, 0x00, sid, b"\x01\x02\x03")
    out += frame(HEADERS, 0, sid, resp(200, body))
    for i in range(len(body)):                        # one byte per DATA frame
        out += frame(DATA, 0, sid, body[i:i + 1])
        out += frame(0xA0 + i % 16, i & 0xFF, sid, b"x" * i)
    out += frame(DATA, END_STREAM, sid, b"")          # empty final DATA
    c.sendall(out)


@test
def client_skips_unknown_frames_and_reassembles():
    srv = FakeServer(grease_handler)
    r = bcurl(f"127.0.0.1:{srv.port}/hello")
    assert r.returncode == 0, r.stderr
    assert r.stdout == b"you asked for /hello\n"


@test
def client_uses_one_connection_for_many_urls():
    srv = FakeServer(grease_handler)
    urls = [f"127.0.0.1:{srv.port}/{n}" for n in ("a", "b", "c", "d")]
    r = bcurl(*urls)
    assert r.returncode == 0, r.stderr
    assert srv.connections == 1, f"{srv.connections} connections"
    assert len(set(srv.streams)) == 4 and 0 not in srv.streams
    assert r.stdout == b"".join(f"you asked for /{n}\n".encode() for n in "abcd")


@test
def client_exit_nonzero_on_4xx_5xx():
    for status in (404, 400, 500, 503):
        def h(c, sid, req, status=status):
            c.sendall(frame(HEADERS, 0, sid, resp(status, b"err\n"))
                      + frame(DATA, END_STREAM, sid, b"err\n"))
        srv = FakeServer(h)
        r = bcurl(f"127.0.0.1:{srv.port}/x")
        assert r.returncode != 0, f"{status}: exit 0"
        assert r.stdout == b"err\n"


@test
def client_headers_only_response():
    srv = FakeServer(lambda c, sid, req: c.sendall(
        frame(HEADERS, END_STREAM, sid, resp(204))))
    r = bcurl(f"127.0.0.1:{srv.port}/")
    assert r.returncode == 0 and r.stdout == b""


@test
def client_detects_truncated_response():
    def h(c, sid, req):
        c.sendall(frame(HEADERS, 0, sid, resp(200, b"0123456789"))
                  + frame(DATA, 0, sid, b"01234"))
        return "close"
    srv = FakeServer(h)
    r = bcurl(f"127.0.0.1:{srv.port}/")
    assert r.returncode not in (0, 22), r.returncode


@test
def client_verbose_dumps_every_frame():
    srv = FakeServer(grease_handler)
    r = bcurl("-v", f"127.0.0.1:{srv.port}/v")
    err = r.stderr.decode()
    assert r.returncode == 0
    assert "PREFACE" in err and "42 48 2f 31 0d 0a 1a 0a" in err
    # 1 request + 2 grease + 1 HEADERS + 2 per body byte + final DATA
    body_len = len(b"you asked for /v\n")
    assert err.count(" frame: ") == 1 + 2 + 1 + 2 * body_len + 1


@test
def end_to_end_with_greasing_bserve():
    port = free_port()
    p = subprocess.Popen([os.path.join(ROOT, "bserve"), "-g", WWW, str(port)],
                         stderr=subprocess.DEVNULL)
    try:
        wait_port(port)
        r = bcurl(f"localhost:{port}/index.html", f"localhost:{port}/big.bin",
                  f"localhost:{port}/nope")
        assert r.returncode == 22
        assert r.stdout.startswith(b"<h1>hi</h1>\n" + BIG)
    finally:
        p.terminate()


# ------------------------------------------------------------------- done

server.terminate()
shutil.rmtree(WWW, ignore_errors=True)
os.remove(os.path.join(os.path.dirname(WWW), "bh-secret.txt"))
print(f"\n{len(PASSED)} passed, {len(FAILED)} failed")
sys.exit(1 if FAILED else 0)
