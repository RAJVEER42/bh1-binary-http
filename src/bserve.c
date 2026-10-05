/* bserve - BH/1 static file server.
 *
 *   usage: bserve [-v] [-g] ROOT PORT
 *     -v  trace every frame to stderr
 *     -g  "grease": send an unknown frame before every response, so clients
 *         that fail to skip unknown frame types are caught early
 */
#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "bserve/1.0"
#define GREASE_TYPE 0xFA

static char  g_root[PATH_MAX];
static size_t g_root_len;
static int   g_grease;
static FILE *g_trace;
static char  g_peer[64];

/* ------------------------------------------------------------ helpers */

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } map[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "text/javascript" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcasecmp(dot, map[i].ext) == 0) return map[i].type;
    return "application/octet-stream";
}

static void http_date(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static int send_grease(int fd, uint32_t stream)
{
    if (!g_grease) return 0;
    static const char junk[] = "skip me";
    return bh_send_frame(fd, GREASE_TYPE, 0x80, stream, junk, sizeof junk - 1,
                         g_trace);
}

/* Response HEADERS with the common fields. */
static int send_head(int fd, uint32_t stream, int status, const char *ctype,
                     long long clen, const char *extra_name,
                     const char *extra_value, int end_stream)
{
    struct bh_buf hb = {0};
    char st[4], cl[32], date[64];
    snprintf(st, sizeof st, "%03d", status);
    snprintf(cl, sizeof cl, "%lld", clen);
    http_date(date, sizeof date);

    int bad = bh_hb_add(&hb, ":status", st) | bh_hb_add(&hb, "content-type", ctype) |
              bh_hb_add(&hb, "content-length", cl) | bh_hb_add(&hb, "server", SERVER_NAME) |
              bh_hb_add(&hb, "date", date);
    if (extra_name) bad |= bh_hb_add(&hb, extra_name, extra_value);

    int r = bad ? -1
                : (send_grease(fd, stream) < 0 ? -1
                   : bh_send_frame(fd, BH_HEADERS, end_stream ? BH_END_STREAM : 0,
                                   stream, hb.p, (uint32_t)hb.len, g_trace));
    bh_buf_free(&hb);
    return r;
}

static int send_error(int fd, uint32_t stream, int status, const char *reason,
                      const char *detail, int head_only)
{
    char body[512];
    int n = snprintf(body, sizeof body, "%d %s%s%s\n", status, reason,
                     detail ? ": " : "", detail ? detail : "");
    fprintf(stderr, "[%s] stream %u -> %d %s%s%s\n", g_peer, stream, status, reason,
            detail ? " (" : "", detail ? detail : "");
    const char *allow_n = status == 405 ? "allow" : NULL;
    if (send_head(fd, stream, status, "text/plain; charset=utf-8", n, allow_n,
                  "GET, HEAD", head_only) < 0)
        return -1;
    if (head_only) return 0;
    return bh_send_frame(fd, BH_DATA, BH_END_STREAM, stream, body, (uint32_t)n,
                         g_trace);
}

static int unhex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Copy :path into out, drop query/fragment, percent-decode. 0 ok, -1 bad. */
static int decode_path(const uint8_t *v, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    if (n == 0 || v[0] != '/') return -1;
    for (size_t i = 0; i < n; i++) {
        int c = v[i];
        if (c == '?' || c == '#') break;
        if (c == '%') {
            if (i + 2 >= n) return -1;
            int hi = unhex(v[i + 1]), lo = unhex(v[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = hi << 4 | lo;
            if (c == 0) return -1;
            i += 2;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return 0;
}

/* Map a decoded URL path to a regular file inside the root. */
static int resolve(const char *path, char *out)
{
    char joined[PATH_MAX * 2];
    size_t plen = strlen(path);
    snprintf(joined, sizeof joined, "%s%s%s", g_root, path,
             path[plen - 1] == '/' ? "index.html" : "");
    if (!realpath(joined, out)) return -1;
    if (strncmp(out, g_root, g_root_len) != 0 ||
        (out[g_root_len] != '/' && out[g_root_len] != '\0'))
        return -1;   /* escaped the root (.. or symlink) */

    struct stat st;
    if (stat(out, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        size_t l = strlen(out);
        if (l + sizeof "/index.html" > PATH_MAX) return -1;
        memcpy(out + l, "/index.html", sizeof "/index.html");
        if (stat(out, &st) < 0) return -1;
    }
    return S_ISREG(st.st_mode) ? 0 : -1;
}

/* ---------------------------------------------------------- a request */

/* Returns 0 to keep the connection, -1 to close it. */
static int handle_request(int fd, struct bh_frame *f)
{
    uint32_t sid = f->stream;
    if (f->discarded)
        return send_error(fd, sid, 400, "Bad Request", "header block too large", 0);
    if (sid == 0)
        return send_error(fd, sid, 400, "Bad Request", "request on stream 0", 0);
    if (!(f->flags & BH_END_STREAM))
        return send_error(fd, sid, 400, "Bad Request", "request bodies not supported", 0);

    struct bh_field fl[BH_MAX_FIELDS];
    const char *why = NULL;
    int n = bh_hb_parse(f->payload, f->len, fl, BH_MAX_FIELDS, &why);
    if (n < 0) return send_error(fd, sid, 400, "Bad Request", why, 0);

    const struct bh_field *method = NULL, *path = NULL;
    int seen_regular = 0;
    for (int i = 0; i < n; i++) {
        if (fl[i].name[0] == ':') {
            if (seen_regular)
                return send_error(fd, sid, 400, "Bad Request", "pseudo-field after regular field", 0);
            const struct bh_field **slot = bh_field_is(&fl[i], ":method") ? &method
                                         : bh_field_is(&fl[i], ":path")   ? &path
                                         : NULL;
            if (!slot)
                return send_error(fd, sid, 400, "Bad Request", "unexpected pseudo-field", 0);
            if (*slot)
                return send_error(fd, sid, 400, "Bad Request", "duplicate pseudo-field", 0);
            *slot = &fl[i];
        } else {
            seen_regular = 1;
        }
    }
    if (!method || !path)
        return send_error(fd, sid, 400, "Bad Request", "missing :method or :path", 0);

    int is_head = method->value_len == 4 && memcmp(method->value, "HEAD", 4) == 0;
    int is_get  = method->value_len == 3 && memcmp(method->value, "GET", 3) == 0;
    if (!is_get && !is_head)
        return send_error(fd, sid, 405, "Method Not Allowed", NULL, 0);

    char upath[PATH_MAX], file[PATH_MAX];
    if (decode_path(path->value, path->value_len, upath, sizeof upath) < 0)
        return send_error(fd, sid, 400, "Bad Request", "bad :path", is_head);
    if (resolve(upath, file) < 0)
        return send_error(fd, sid, 404, "Not Found", NULL, is_head);

    int ffd = open(file, O_RDONLY);
    struct stat st;
    if (ffd < 0 || fstat(ffd, &st) < 0) {
        if (ffd >= 0) close(ffd);
        return send_error(fd, sid, 404, "Not Found", NULL, is_head);
    }

    long long size = st.st_size;
    int no_body = is_head || size == 0;
    fprintf(stderr, "[%s] stream %u %s %s -> 200 (%lld bytes)\n", g_peer, sid,
            is_head ? "HEAD" : "GET", upath, size);
    if (send_head(fd, sid, 200, mime_type(file), size, NULL, NULL, no_body) < 0) {
        close(ffd);
        return -1;
    }
    if (no_body) { close(ffd); return 0; }

    static uint8_t chunk[BH_DATA_CHUNK];
    long long left = size;
    while (left > 0) {
        size_t want = left < (long long)sizeof chunk ? (size_t)left : sizeof chunk;
        ssize_t r = read(ffd, chunk, want);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            /* File shrank under us. We promised `size` bytes and cannot
             * take that back, so per spec we close the connection. */
            close(ffd);
            return -1;
        }
        left -= r;
        if (bh_send_frame(fd, BH_DATA, left == 0 ? BH_END_STREAM : 0, sid, chunk,
                          (uint32_t)r, g_trace) < 0) {
            close(ffd);
            return -1;
        }
    }
    close(ffd);
    return 0;
}

/* ------------------------------------------------------ a connection */

static void serve_connection(int fd)
{
    uint8_t pre[BH_PREFACE_LEN];
    if (bh_read_all(fd, pre, sizeof pre) != 1) return;
    if (g_trace) bh_trace_preface(g_trace, "<", pre);
    if (memcmp(pre, BH_PREFACE, BH_PREFACE_LEN) != 0) {
        fprintf(stderr, "[%s] bad preface, closing\n", g_peer);
        return;   /* not speaking BH/1: we cannot even frame a reply */
    }

    for (;;) {
        struct bh_frame f;
        int r = bh_recv_frame(fd, &f, BH_MAX_HEADER_BLOCK, g_trace);
        if (r == 0) break;                 /* clean close between frames */
        if (r < 0) {
            fprintf(stderr, "[%s] connection lost mid-frame\n", g_peer);
            break;
        }
        int keep = 0;
        if (f.type == BH_HEADERS)
            keep = handle_request(fd, &f);
        /* DATA: v1 requests carry no body, ignore.
         * Anything else: unknown type, MUST be skipped - it already was,
         * bh_recv_frame consumed exactly f.len bytes. */
        bh_frame_free(&f);
        if (keep < 0) break;
    }
    fprintf(stderr, "[%s] closed\n", g_peer);
}

static int listen_on(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1, zero = 0;
    if (fd >= 0) {
        struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons(port),
                                   .sin6_addr = in6addr_any };
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(fd, 64) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a4, sizeof a4) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void peer_name(struct sockaddr_storage *ss)
{
    char host[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (ss->ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        port = ntohs(a->sin6_port);
    } else if (ss->ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        port = ntohs(a->sin_port);
    }
    snprintf(g_peer, sizeof g_peer, "%s:%d", host, port);
}

static void usage(void)
{
    fprintf(stderr, "usage: bserve [-v] [-g] ROOT PORT\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "vg")) != -1) {
        if (opt == 'v') g_trace = stderr;
        else if (opt == 'g') g_grease = 1;
        else usage();
    }
    if (argc - optind != 2) usage();

    if (!realpath(argv[optind], g_root)) {
        perror(argv[optind]);
        return 1;
    }
    g_root_len = strlen(g_root);
    if (g_root_len == 1) g_root_len = 0;   /* root is "/" */

    char *end;
    long port = strtol(argv[optind + 1], &end, 10);
    if (*end || port < 1 || port > 65535) usage();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* auto-reap connection children */

    int lfd = listen_on((int)port);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "bserve: serving %s on port %ld (BH/1)%s\n", g_root, port,
            g_grease ? " [grease on]" : "");

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        peer_name(&ss);
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            fprintf(stderr, "[%s] connected\n", g_peer);
            serve_connection(cfd);
            close(cfd);
            _exit(0);
        }
        if (pid < 0) perror("fork");
        close(cfd);
    }
}
