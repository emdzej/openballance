/* DEFLATE decoder (RFC 1951). Canonical Huffman tables with a 9-bit lookup table for the short codes
   (literal/length codes of 9 bits or less cover nearly every symbol) and the per-length count walk
   for the rest; bits come from a 64-bit buffer refilled bytewise. The CD's cabinets hold ~150 MB of
   deflated game files, so the decoder's speed is the load time when running from the CD. */
#include "inflate.h"
#include <string.h>

enum { FAST = 9 };

typedef struct {
    const uint8_t *src;
    size_t len, pos;
    uint64_t bits;
    int nbits;
    uint8_t *dst;
    size_t cap, out;
    int error;
} Inf;

typedef struct {
    uint16_t count[16];      /* codes per length */
    uint16_t symbol[320];    /* symbols ordered by code */
    uint16_t fast[1 << FAST];/* next FAST bits (LSB first) -> symbol << 4 | length; 0 = longer code */
} Huff;

static void refill(Inf *s)
{
    while (s->nbits <= 56 && s->pos < s->len) {
        s->bits |= (uint64_t)s->src[s->pos++] << s->nbits;
        s->nbits += 8;
    }
}

static unsigned getbits(Inf *s, int n)
{
    if (!n) return 0;
    if (s->nbits < n) {
        refill(s);
        if (s->nbits < n) { s->error = 1; return 0; }
    }
    unsigned v = (unsigned)(s->bits & ((1u << n) - 1));
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

static int build(Huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return -1;              /* over-subscribed */
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    /* fast table: canonical codes in symbol order, bit-reversed (the stream sends them MSB first) */
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l <= FAST; l++, code <<= 1)
        for (int c = 0; c < h->count[l]; c++, code++, k++) {
            unsigned r = 0;
            for (int b = 0; b < l; b++) r |= ((code >> b) & 1) << (l - 1 - b);
            for (unsigned x = r; x < (1u << FAST); x += 1u << l) h->fast[x] = (uint16_t)(h->symbol[k] << 4 | l);
        }
    return 0;
}

static int decode(Inf *s, const Huff *h)
{
    if (s->nbits < 15) refill(s);
    if (s->nbits >= FAST) {
        unsigned e = h->fast[s->bits & ((1u << FAST) - 1)];
        if (e) {
            s->bits >>= e & 15;
            s->nbits -= e & 15;
            return e >> 4;
        }
    }
    int code = 0, first = 0, index = 0;   /* bit by bit: long codes, or the last bits of the input */
    for (int l = 1; l < 16; l++) {
        code |= (int)getbits(s, 1);
        if (s->error) return 0;
        int c = h->count[l];
        if (code - c < first) return h->symbol[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    s->error = 1;
    return 0;
}

static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DEXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inf *s, const Huff *lit, const Huff *dist)
{
    uint8_t *dst = s->dst;
    for (;;) {
        int sym = decode(s, lit);
        if (s->error) return -1;
        if (sym < 256) {
            if (s->out >= s->cap) return -1;
            dst[s->out++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t n = LBASE[sym] + getbits(s, LEXTRA[sym]);
            int d = decode(s, dist);
            if (s->error || d >= 30) return -1;
            size_t back = DBASE[d] + getbits(s, DEXTRA[d]);
            if (s->error || back > s->out || s->out + n > s->cap) return -1;
            uint8_t *o = dst + s->out;
            const uint8_t *from = o - back;
            if (back >= n) memcpy(o, from, n);
            else for (size_t i = 0; i < n; i++) o[i] = from[i];   /* overlapping: a repeating run */
            s->out += n;
        }
    }
}

static int fixed_block(Inf *s)
{
    static Huff lit, dist;
    static int ready;
    if (!ready) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(&dist, l, 30);
        ready = 1;
    }
    return codes(s, &lit, &dist);
}

static int dynamic_block(Inf *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t len[320] = {0};
    Huff lencode, lit, dist;
    int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)getbits(s, 3);
    if (s->error || build(&lencode, len, 19)) return -1;
    memset(len, 0, sizeof len);
    for (int i = 0; i < nlen + ndist;) {
        int sym = decode(s, &lencode);
        if (s->error) return -1;
        if (sym < 16) {
            len[i++] = (uint8_t)sym;
        } else {
            int v = 0, rep;
            if (sym == 16) {
                if (!i) return -1;
                v = len[i - 1];
                rep = 3 + (int)getbits(s, 2);
            } else if (sym == 17) {
                rep = 3 + (int)getbits(s, 3);
            } else {
                rep = 11 + (int)getbits(s, 7);
            }
            if (i + rep > nlen + ndist) return -1;
            while (rep--) len[i++] = (uint8_t)v;
        }
    }
    if (build(&lit, len, nlen) || build(&dist, len + nlen, ndist)) return -1;
    return codes(s, &lit, &dist);
}

/* Bytes not yet consumed: the buffered whole bytes go back to the input. */
static void unread(Inf *s)
{
    s->pos -= (size_t)(s->nbits >> 3);
    s->bits = 0;
    s->nbits = 0;
}

static int stored_block(Inf *s)
{
    s->bits >>= s->nbits & 7;                          /* byte align */
    s->nbits &= ~7;
    unread(s);
    if (s->pos + 4 > s->len) return -1;
    unsigned n = s->src[s->pos] | s->src[s->pos + 1] << 8;
    unsigned nn = s->src[s->pos + 2] | s->src[s->pos + 3] << 8;
    s->pos += 4;
    if ((n ^ 0xffff) != nn || s->pos + n > s->len || s->out + n > s->cap) return -1;
    memcpy(s->dst + s->out, s->src + s->pos, n);
    s->pos += n;
    s->out += n;
    return 0;
}

/* open_end: the stream may also end, without a final block, where the input ends on a block boundary
   (a sync flush ends byte aligned, so that is when every input byte has been used). */
static long inflate_blocks(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *consumed, int open_end)
{
    Inf s = {.src = src, .len = len, .dst = dst, .cap = cap};
    int last = 0;
    do {
        if (open_end && s.pos >= s.len && s.nbits < 8) break;
        last = (int)getbits(&s, 1);
        int type = (int)getbits(&s, 2);
        int r = s.error ? -1 : type == 0 ? stored_block(&s) : type == 1 ? fixed_block(&s) : type == 2 ? dynamic_block(&s) : -1;
        if (r || s.error) return -1;
    } while (!last);
    unread(&s);
    if (consumed) *consumed = s.pos;
    return (long)s.out;
}

long deflate_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *consumed)
{
    return inflate_blocks(src, len, dst, cap, consumed, 0);
}

long deflate_inflate_flushed(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    return inflate_blocks(src, len, dst, cap, NULL, 1);
}

long zlib_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    if (len < 6 || (src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 || (src[1] & 0x20)) return -1;
    size_t used;
    long n = deflate_inflate(src + 2, len - 2, dst, cap, &used);
    if (n < 0 || 2 + used + 4 > len) return -1;
    uint32_t a = 1, b = 0;                              /* Adler-32, reduced every 5552 bytes */
    for (long i = 0; i < n;) {
        long end = i + 5552 < n ? i + 5552 : n;
        for (; i < end; i++) a += dst[i], b += a;
        a %= 65521;
        b %= 65521;
    }
    const uint8_t *t = src + 2 + used;
    uint32_t want = (uint32_t)t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];
    return ((b << 16) | a) == want ? n : -1;
}
