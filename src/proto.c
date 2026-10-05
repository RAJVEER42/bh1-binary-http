/* proto.c - BH/1 wire format shared by bserve and bcurl. */
#include "proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/uio.h>
#include <unistd.h>

const char *const bh_static_names[BH_STATIC_COUNT + 1] = {
    NULL,               /* 0: literal name follows */
    ":method",          /* 1 */
    ":path",            /* 2 */
    ":status",          /* 3 */
    "host",             /* 4 */
    "user-agent",       /* 5 */
    "accept",           /* 6 */
    "content-type",     /* 7 */
    "content-length",   /* 8 */
    "server",           /* 9 */
    "date",             /* 10 */
};

/* ---------------------------------------------------------------- I/O */

int bh_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int bh_read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return got == 0 ? 0 : -1;
        got += (size_t)r;
    }
    return 1;
}

/* ------------------------------------------------------------ framing */

void bh_pack_header(uint8_t out[BH_HDR_LEN], uint32_t len, uint8_t type,
                    uint8_t flags, uint32_t stream)
{
    out[0] = (uint8_t)(len >> 16);
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    out[3] = type;
    out[4] = flags;
    out[5] = (uint8_t)(stream >> 16);
    out[6] = (uint8_t)(stream >> 8);
    out[7] = (uint8_t)stream;
}

void bh_unpack_header(const uint8_t in[BH_HDR_LEN], struct bh_frame *f)
{
    f->len    = (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
    f->type   = in[3];
    f->flags  = in[4];
    f->stream = (uint32_t)in[5] << 16 | (uint32_t)in[6] << 8 | in[7];
}

int bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                  const void *payload, uint32_t len, FILE *trace)
{
    uint8_t hdr[BH_HDR_LEN];
    if (len > BH_MAX_LEN || stream > BH_MAX_STREAM) return -1;
    bh_pack_header(hdr, len, type, flags, stream);
    if (trace) bh_trace_frame(trace, ">", hdr, payload, len, 0);

    struct iovec iov[2] = {
        { .iov_base = hdr, .iov_len = sizeof hdr },
        { .iov_base = (void *)payload, .iov_len = len },
    };
    size_t total = sizeof hdr + len;
    int cnt = len ? 2 : 1;
    ssize_t w = writev(fd, iov, cnt);
    if (w < 0 && errno != EINTR) return -1;
    if (w < 0) w = 0;
    if ((size_t)w == total) return 0;
    /* Short write: finish the rest the slow way. */
    if ((size_t)w < sizeof hdr) {
        if (bh_write_all(fd, hdr + w, sizeof hdr - (size_t)w) < 0) return -1;
        return len ? bh_write_all(fd, payload, len) : 0;
    }
    return bh_write_all(fd, (const uint8_t *)payload + (w - (ssize_t)sizeof hdr),
                        total - (size_t)w);
}

int bh_recv_frame(int fd, struct bh_frame *f, uint32_t max_buffer, FILE *trace)
{
    uint8_t hdr[BH_HDR_LEN];
    memset(f, 0, sizeof *f);
    int r = bh_read_all(fd, hdr, sizeof hdr);
    if (r <= 0) return r;
    bh_unpack_header(hdr, f);

    if (f->len > max_buffer) {
        /* Too big to hold: read it and throw it away, keeping sync. */
        uint8_t sink[4096];
        uint32_t left = f->len;
        while (left > 0) {
            uint32_t n = left < sizeof sink ? left : (uint32_t)sizeof sink;
            if (bh_read_all(fd, sink, n) != 1) return -1;
            left -= n;
        }
        f->discarded = 1;
    } else if (f->len > 0) {
        f->payload = malloc(f->len);
        if (!f->payload) return -1;
        if (bh_read_all(fd, f->payload, f->len) != 1) {
            free(f->payload);
            f->payload = NULL;
            return -1;
        }
    }
    if (trace) bh_trace_frame(trace, "<", hdr, f->payload, f->len, f->discarded);
    return 1;
}

void bh_frame_free(struct bh_frame *f)
{
    free(f->payload);
    f->payload = NULL;
}

/* ------------------------------------------------------- header blocks */

static int buf_reserve(struct bh_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

void bh_buf_free(struct bh_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static int static_index(const char *name)
{
    for (int i = 1; i <= BH_STATIC_COUNT; i++)
        if (strcasecmp(name, bh_static_names[i]) == 0) return i;
    return 0;
}

int bh_hb_add_n(struct bh_buf *b, const char *name, const void *value, size_t vlen)
{
    int idx = static_index(name);
    size_t nlen = strlen(name);
    if (vlen > 0xFFFF || (!idx && (nlen == 0 || nlen > 0xFF))) return -1;
    if (buf_reserve(b, 1 + 1 + nlen + 2 + vlen) < 0) return -1;

    b->p[b->len++] = (uint8_t)idx;
    if (!idx) {
        b->p[b->len++] = (uint8_t)nlen;
        for (size_t i = 0; i < nlen; i++)
            b->p[b->len++] = (uint8_t)tolower((unsigned char)name[i]);
    }
    b->p[b->len++] = (uint8_t)(vlen >> 8);
    b->p[b->len++] = (uint8_t)vlen;
    memcpy(b->p + b->len, value, vlen);
    b->len += vlen;
    return 0;
}

int bh_hb_add(struct bh_buf *b, const char *name, const char *value)
{
    return bh_hb_add_n(b, name, value, strlen(value));
}

int bh_hb_parse(const uint8_t *p, size_t len, struct bh_field *out, int max,
                const char **why)
{
    size_t off = 0;
    int n = 0;
    while (off < len) {
        struct bh_field f = {0};
        if (n == max) { *why = "too many fields"; return -1; }

        f.index = p[off++];
        if (f.index == 0) {
            if (off + 1 > len) { *why = "truncated name length"; return -1; }
            f.name_len = p[off++];
            if (f.name_len == 0) { *why = "empty literal name"; return -1; }
            if (off + f.name_len > len) { *why = "truncated name"; return -1; }
            f.name = (const char *)p + off;
            for (size_t i = 0; i < f.name_len; i++) {
                unsigned char c = (unsigned char)f.name[i];
                if (c < 0x21 || c > 0x7E || isupper(c) || c == ':') {
                    *why = "bad character in literal name";
                    return -1;
                }
            }
            off += f.name_len;
        } else if (f.index <= BH_STATIC_COUNT) {
            f.name = bh_static_names[f.index];
            f.name_len = strlen(f.name);
        } else {
            *why = "unknown static index";
            return -1;
        }

        if (off + 2 > len) { *why = "truncated value length"; return -1; }
        f.value_len = (size_t)p[off] << 8 | p[off + 1];
        off += 2;
        if (off + f.value_len > len) { *why = "truncated value"; return -1; }
        f.value = p + off;
        for (size_t i = 0; i < f.value_len; i++) {
            if (f.value[i] == 0 || f.value[i] == '\r' || f.value[i] == '\n') {
                *why = "NUL/CR/LF in value";
                return -1;
            }
        }
        off += f.value_len;
        out[n++] = f;
    }
    return n;
}

int bh_field_is(const struct bh_field *f, const char *name)
{
    return strlen(name) == f->name_len && memcmp(f->name, name, f->name_len) == 0;
}

/* --------------------------------------------------------- diagnostics */

const char *bh_type_name(uint8_t type)
{
    switch (type) {
    case BH_DATA:    return "DATA";
    case BH_HEADERS: return "HEADERS";
    default:         return "UNKNOWN";
    }
}

void bh_hexdump(FILE *out, const char *prefix, const uint8_t *p, size_t n,
                size_t base)
{
    for (size_t i = 0; i < n; i += 16) {
        fprintf(out, "%s  %04zx  ", prefix, base + i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) fprintf(out, "%02x ", p[i + j]);
            else           fputs("   ", out);
            if (j == 7) fputc(' ', out);
        }
        fputs(" |", out);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            fputc(isprint(p[i + j]) ? p[i + j] : '.', out);
        fputs("|\n", out);
    }
}

void bh_trace_preface(FILE *out, const char *dir, const uint8_t *p)
{
    fprintf(out, "%s PREFACE (8 bytes)\n", dir);
    bh_hexdump(out, dir, p, BH_PREFACE_LEN, 0);
}

void bh_trace_frame(FILE *out, const char *dir, const uint8_t hdr[BH_HDR_LEN],
                    const uint8_t *payload, uint32_t len, int discarded)
{
    struct bh_frame f;
    bh_unpack_header(hdr, &f);
    fprintf(out, "%s %s frame: type=0x%02x len=%u flags=0x%02x%s stream=%u\n",
            dir, bh_type_name(f.type), f.type, f.len, f.flags,
            (f.flags & BH_END_STREAM) ? " [END_STREAM]" : "", f.stream);

    /* Dump header and payload as one contiguous frame. */
    uint8_t first[16];
    size_t lead = len < 8 ? len : 8;
    memcpy(first, hdr, BH_HDR_LEN);
    if (payload && lead) memcpy(first + BH_HDR_LEN, payload, lead);
    bh_hexdump(out, dir, first, BH_HDR_LEN + (payload ? lead : 0), 0);
    if (payload && len > lead)
        bh_hexdump(out, dir, payload + lead, len - lead, 16);
    if (discarded)
        fprintf(out, "%s  (payload of %u bytes over limit: read and discarded)\n",
                dir, len);

    if (f.type == BH_HEADERS && payload) {
        struct bh_field fields[BH_MAX_FIELDS];
        const char *why = NULL;
        int n = bh_hb_parse(payload, len, fields, BH_MAX_FIELDS, &why);
        if (n < 0) {
            fprintf(out, "%s  ! malformed header block: %s\n", dir, why);
        } else {
            for (int i = 0; i < n; i++) {
                if (fields[i].index)
                    fprintf(out, "%s    [idx %2d] ", dir, fields[i].index);
                else
                    fprintf(out, "%s    [literal] ", dir);
                fprintf(out, "%.*s: %.*s\n", (int)fields[i].name_len,
                        fields[i].name, (int)fields[i].value_len,
                        (const char *)fields[i].value);
            }
        }
    } else if (f.type != BH_DATA && f.type != BH_HEADERS) {
        fprintf(out, "%s  (unknown frame type 0x%02x: skipped)\n", dir, f.type);
    }
}
