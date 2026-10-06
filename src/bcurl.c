/* bcurl - BH/1 client.
 *
 *   usage: bcurl [-v] [-I] [-i] URL [URL...]
 *     URL  [bh://]host[:port][/path]   (default port 9000, default path /)
 *     -v   hexdump every frame sent and received to stderr
 *     -I   send HEAD instead of GET, print the response fields
 *     -i   print the response fields before the body
 *
 *   Several URLs are fetched in order over ONE connection; they must all
 *   name the same host:port. Bodies go to stdout.
 *
 *   exit: 0 ok, 22 any response was 4xx/5xx, 2 usage, 7 cannot connect,
 *         8 protocol error / connection lost.
 */
#include "proto.h"

#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USER_AGENT "bcurl/1.0"

struct url {
    char host[256];
    char port[8];
    char path[4096];
    char authority[300];
};

static FILE *g_trace;

static void usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] [-I] [-i] [bh://]host[:port][/path] ...\n");
    exit(2);
}

static int parse_url(const char *s, struct url *u)
{
    if (strncmp(s, "bh://", 5) == 0) s += 5;
    else if (strstr(s, "://")) return -1;

    const char *slash = strchr(s, '/');
    size_t alen = slash ? (size_t)(slash - s) : strlen(s);
    if (alen == 0 || alen >= sizeof u->authority) return -1;
    memcpy(u->authority, s, alen);
    u->authority[alen] = '\0';

    const char *hs = u->authority, *colon;
    size_t hlen;
    if (hs[0] == '[') {                       /* [v6addr]:port */
        const char *rb = strchr(hs, ']');
        if (!rb) return -1;
        hlen = (size_t)(rb - hs - 1);
        memcpy(u->host, hs + 1, hlen);
        colon = rb[1] == ':' ? rb + 1 : NULL;
    } else {
        colon = strrchr(hs, ':');
        hlen = colon ? (size_t)(colon - hs) : alen;
        memcpy(u->host, hs, hlen);
    }
    if (hlen == 0 || hlen >= sizeof u->host) return -1;
    u->host[hlen] = '\0';
    snprintf(u->port, sizeof u->port, "%s", colon ? colon + 1 : "9000");

    snprintf(u->path, sizeof u->path, "%s", slash ? slash : "/");
    return 0;
}

static int dial(const struct url *u)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    int e = getaddrinfo(u->host, u->port, &hints, &res);
    if (e) {
        fprintf(stderr, "bcurl: %s: %s\n", u->host, gai_strerror(e));
        return -1;
    }
    /* Try each address until ONE connects; that is the only connection
     * this process will ever use. */
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "bcurl: cannot connect to %s\n", u->authority);
    return fd;
}

static int send_request(int fd, uint32_t sid, const struct url *u, int head)
{
    struct bh_buf hb = {0};
    int bad = bh_hb_add(&hb, ":method", head ? "HEAD" : "GET") |
              bh_hb_add(&hb, ":path", u->path) |
              bh_hb_add(&hb, "host", u->authority) |
              bh_hb_add(&hb, "user-agent", USER_AGENT) |
              bh_hb_add(&hb, "accept", "*/*");
    int r = bad ? -1 : bh_send_frame(fd, BH_HEADERS, BH_END_STREAM, sid, hb.p,
                                     (uint32_t)hb.len, g_trace);
    bh_buf_free(&hb);
    return r;
}

/* Read one complete response on stream sid. Returns the status code,
 * or -1 on a protocol error / lost connection. */
static int read_response(int fd, uint32_t sid, int head, int show_fields)
{
    int status = 0;
    long long clen = -1, got = 0;

    for (;;) {
        struct bh_frame f;
        int r = bh_recv_frame(fd, &f, BH_MAX_LEN, g_trace);
        if (r <= 0) {
            fprintf(stderr, "bcurl: connection closed before response completed\n");
            return -1;
        }
        /* Unknown types MUST be skipped; v1 has one request in flight, so
         * a frame for another stream is not ours and is skipped too. */
        if ((f.type != BH_DATA && f.type != BH_HEADERS) || f.stream != sid) {
            bh_frame_free(&f);
            continue;
        }

        if (f.type == BH_HEADERS) {
            if (status) {
                fprintf(stderr, "bcurl: second HEADERS frame on stream %u\n", sid);
                bh_frame_free(&f);
                return -1;
            }
            struct bh_field fl[BH_MAX_FIELDS];
            const char *why = NULL;
            int n = bh_hb_parse(f.payload, f.len, fl, BH_MAX_FIELDS, &why);
            if (n < 1 || !bh_field_is(&fl[0], ":status") || fl[0].value_len != 3) {
                fprintf(stderr, "bcurl: bad response header block: %s\n",
                        n < 0 ? why : "first field is not a 3-digit :status");
                bh_frame_free(&f);
                return -1;
            }
            status = (fl[0].value[0] - '0') * 100 + (fl[0].value[1] - '0') * 10 +
                     (fl[0].value[2] - '0');
            if (status < 100 || status > 599) {
                fprintf(stderr, "bcurl: bad :status\n");
                bh_frame_free(&f);
                return -1;
            }
            for (int i = 0; i < n; i++) {
                if (bh_field_is(&fl[i], "content-length")) {
                    char tmp[32] = {0};
                    memcpy(tmp, fl[i].value, fl[i].value_len < 31 ? fl[i].value_len : 31);
                    clen = strtoll(tmp, NULL, 10);
                }
                if (show_fields)
                    printf("%.*s: %.*s\n", (int)fl[i].name_len, fl[i].name,
                           (int)fl[i].value_len, (const char *)fl[i].value);
            }
            if (show_fields) putchar('\n');
            if (g_trace) fprintf(g_trace, "* stream %u: status %d\n", sid, status);
        } else { /* DATA */
            if (!status) {
                fprintf(stderr, "bcurl: DATA before HEADERS on stream %u\n", sid);
                bh_frame_free(&f);
                return -1;
            }
            if (f.len) fwrite(f.payload, 1, f.len, stdout);
            got += f.len;
        }

        int end = f.flags & BH_END_STREAM;
        bh_frame_free(&f);
        if (end) break;
    }
    fflush(stdout);
    if (!head && clen >= 0 && clen != got)
        fprintf(stderr, "bcurl: warning: content-length %lld but received %lld bytes\n",
                clen, got);
    return status;
}

int main(int argc, char **argv)
{
    int opt, head = 0, show = 0;
    while ((opt = getopt(argc, argv, "vIi")) != -1) {
        if (opt == 'v') g_trace = stderr;
        else if (opt == 'I') head = show = 1;
        else if (opt == 'i') show = 1;
        else usage();
    }
    int nurl = argc - optind;
    if (nurl < 1) usage();

    struct url *urls = calloc((size_t)nurl, sizeof *urls);
    for (int i = 0; i < nurl; i++) {
        if (parse_url(argv[optind + i], &urls[i]) < 0) {
            fprintf(stderr, "bcurl: bad URL: %s\n", argv[optind + i]);
            return 2;
        }
        if (strcmp(urls[i].host, urls[0].host) || strcmp(urls[i].port, urls[0].port)) {
            fprintf(stderr, "bcurl: all URLs must share one host:port "
                            "(bcurl never opens a second connection)\n");
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    int fd = dial(&urls[0]);
    if (fd < 0) return 7;

    if (g_trace) bh_trace_preface(g_trace, ">", (const uint8_t *)BH_PREFACE);
    if (bh_write_all(fd, BH_PREFACE, BH_PREFACE_LEN) < 0) {
        perror("bcurl: write");
        return 8;
    }

    int failed = 0;
    for (int i = 0; i < nurl; i++) {
        uint32_t sid = (uint32_t)i + 1;   /* streams: 1, 2, 3, ... */
        if (send_request(fd, sid, &urls[i], head) < 0) {
            fprintf(stderr, "bcurl: failed to send request\n");
            return 8;
        }
        int status = read_response(fd, sid, head, show);
        if (status < 0) return 8;
        if (status >= 400) {
            fprintf(stderr, "bcurl: %s%s returned %d\n", urls[i].authority,
                    urls[i].path, status);
            failed = 1;
        }
    }
    close(fd);
    free(urls);
    return failed ? 22 : 0;
}
