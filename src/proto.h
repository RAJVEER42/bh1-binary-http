/* proto.h - BH/1 wire format: framing, header blocks, tracing. See SPEC.md. */
#ifndef BH_PROTO_H
#define BH_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BH_PREFACE          "BH/1\r\n\x1a\n"
#define BH_PREFACE_LEN      8
#define BH_HDR_LEN          8
#define BH_MAX_LEN          0xFFFFFFu     /* 24-bit length field          */
#define BH_MAX_STREAM       0xFFFFFFu     /* 24-bit stream field          */
#define BH_MAX_HEADER_BLOCK 65536u        /* receivers MAY reject larger  */
#define BH_DATA_CHUNK       16384u        /* what we send per DATA frame  */
#define BH_MAX_FIELDS       64

/* Frame types */
#define BH_DATA     0x00
#define BH_HEADERS  0x01

/* Flags */
#define BH_END_STREAM 0x01

/* Static name table: index 1..10. Index 0 means "literal name follows". */
#define BH_STATIC_COUNT 10
extern const char *const bh_static_names[BH_STATIC_COUNT + 1];

struct bh_frame {
    uint32_t len;          /* payload length from the header            */
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream;
    uint8_t *payload;      /* malloc'd, or NULL if len == 0 / discarded */
    int      discarded;    /* payload was larger than caller's limit    */
};

struct bh_field {
    int            index;  /* static index, or 0 for a literal name */
    const char    *name;   /* not NUL-terminated                    */
    size_t         name_len;
    const uint8_t *value;  /* not NUL-terminated                    */
    size_t         value_len;
};

struct bh_buf {
    uint8_t *p;
    size_t   len, cap;
};

/* I/O. bh_read_all returns 1 ok, 0 clean EOF before any byte, -1 error/short. */
int  bh_write_all(int fd, const void *buf, size_t n);
int  bh_read_all(int fd, void *buf, size_t n);

void bh_pack_header(uint8_t out[BH_HDR_LEN], uint32_t len, uint8_t type,
                    uint8_t flags, uint32_t stream);
void bh_unpack_header(const uint8_t in[BH_HDR_LEN], struct bh_frame *f);

/* Send one frame. trace may be NULL. */
int  bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                   const void *payload, uint32_t len, FILE *trace);

/* Receive one frame. Payloads longer than max_buffer are read and thrown
 * away (f->discarded = 1). Returns 1 ok, 0 clean EOF, -1 error/truncated. */
int  bh_recv_frame(int fd, struct bh_frame *f, uint32_t max_buffer, FILE *trace);
void bh_frame_free(struct bh_frame *f);

/* Header blocks. */
void bh_buf_free(struct bh_buf *b);
int  bh_hb_add(struct bh_buf *b, const char *name, const char *value);
int  bh_hb_add_n(struct bh_buf *b, const char *name, const void *value, size_t vlen);
/* Returns number of fields parsed, or -1 if the block is malformed;
 * *why receives a short reason on failure. */
int  bh_hb_parse(const uint8_t *p, size_t len, struct bh_field *out, int max,
                 const char **why);
int  bh_field_is(const struct bh_field *f, const char *name);

/* Diagnostics. */
const char *bh_type_name(uint8_t type);
void bh_hexdump(FILE *out, const char *prefix, const uint8_t *p, size_t n,
                size_t base);
void bh_trace_preface(FILE *out, const char *dir, const uint8_t *p);
void bh_trace_frame(FILE *out, const char *dir, const uint8_t hdr[BH_HDR_LEN],
                    const uint8_t *payload, uint32_t len, int discarded);

#endif
