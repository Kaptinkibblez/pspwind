/*
 * inflate.c - raw DEFLATE decoder (stored, fixed and dynamic blocks).
 *
 * Canonical Huffman decoding by code-length counts (the classic "puff"
 * scheme): slower than table lookup but tiny, and text blocks are only
 * decoded when the dialogue or book UI needs a string that is not cached.
 */
#include "inflate.h"

#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[FIXLCODES];
} Huffman;

typedef struct {
    const uint8_t *src;
    uint32_t src_len, src_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *dst;
    uint32_t dst_cap, dst_pos;
    int err;
} State;

static uint32_t bits(State *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->src_pos >= s->src_len) {
            s->err = PW_INFLATE_ERR_INPUT;
            return 0;
        }
        val |= (uint32_t)s->src[s->src_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1u);
}

static int stored(State *s)
{
    uint32_t len, nlen;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->src_pos + 4 > s->src_len)
        return PW_INFLATE_ERR_INPUT;
    len = s->src[s->src_pos] | ((uint32_t)s->src[s->src_pos + 1] << 8);
    nlen = s->src[s->src_pos + 2] | ((uint32_t)s->src[s->src_pos + 3] << 8);
    s->src_pos += 4;
    if (len != (~nlen & 0xFFFFu))
        return PW_INFLATE_ERR_DATA;
    if (s->src_pos + len > s->src_len)
        return PW_INFLATE_ERR_INPUT;
    if (s->dst_pos + len > s->dst_cap)
        return PW_INFLATE_ERR_OUTPUT;
    memcpy(s->dst + s->dst_pos, s->src + s->src_pos, len);
    s->dst_pos += len;
    s->src_pos += len;
    return PW_INFLATE_OK;
}

static int decode(State *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAXBITS; ++len) {
        int count;
        code |= (int)bits(s, 1);
        if (s->err)
            return -1;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = PW_INFLATE_ERR_DATA;
    return -1;
}

/* Returns 0 for a complete code, >0 for an incomplete one (allowed only for
 * single-code distance tables), <0 when over-subscribed. */
static int construct(Huffman *h, const uint8_t *length, int n)
{
    uint16_t offs[MAXBITS + 1];
    int len, sym, left;
    memset(h->count, 0, sizeof(h->count));
    for (sym = 0; sym < n; ++sym)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; ++len)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; ++sym)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    return left;
}

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(State *s, const Huffman *lencode, const Huffman *distcode)
{
    for (;;) {
        int symbol = decode(s, lencode);
        if (s->err)
            return s->err;
        if (symbol < 256) {
            if (s->dst_pos >= s->dst_cap)
                return PW_INFLATE_ERR_OUTPUT;
            s->dst[s->dst_pos++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return PW_INFLATE_OK;
        } else {
            uint32_t len, dist;
            symbol -= 257;
            if (symbol >= 29)
                return PW_INFLATE_ERR_DATA;
            len = lbase[symbol] + bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (s->err)
                return s->err;
            if (symbol < 0 || symbol >= 30)
                return PW_INFLATE_ERR_DATA;
            dist = dbase[symbol] + bits(s, dext[symbol]);
            if (s->err)
                return s->err;
            if (dist > s->dst_pos)
                return PW_INFLATE_ERR_DATA;
            if (s->dst_pos + len > s->dst_cap)
                return PW_INFLATE_ERR_OUTPUT;
            while (len--) {
                s->dst[s->dst_pos] = s->dst[s->dst_pos - dist];
                s->dst_pos++;
            }
        }
    }
}

static int fixed(State *s)
{
    static Huffman lencode, distcode;
    static int built = 0;
    if (!built) {
        uint8_t lengths[FIXLCODES];
        int sym;
        for (sym = 0; sym < 144; ++sym)
            lengths[sym] = 8;
        for (; sym < 256; ++sym)
            lengths[sym] = 9;
        for (; sym < 280; ++sym)
            lengths[sym] = 7;
        for (; sym < FIXLCODES; ++sym)
            lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; ++sym)
            lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(State *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[MAXLCODES + MAXDCODES];
    Huffman lencode, distcode;
    int nlen, ndist, ncode, index, err;
    nlen = (int)bits(s, 5) + 257;
    ndist = (int)bits(s, 5) + 1;
    ncode = (int)bits(s, 4) + 4;
    if (s->err)
        return s->err;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return PW_INFLATE_ERR_DATA;
    for (index = 0; index < ncode; ++index)
        lengths[order[index]] = (uint8_t)bits(s, 3);
    for (; index < 19; ++index)
        lengths[order[index]] = 0;
    if (s->err)
        return s->err;
    if (construct(&lencode, lengths, 19) != 0)
        return PW_INFLATE_ERR_DATA;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode), len = 0, rep;
        if (s->err)
            return s->err;
        if (symbol < 16) {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 16) {
            if (index == 0)
                return PW_INFLATE_ERR_DATA;
            len = lengths[index - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (symbol == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (s->err)
            return s->err;
        if (index + rep > nlen + ndist)
            return PW_INFLATE_ERR_DATA;
        while (rep--)
            lengths[index++] = (uint8_t)len;
    }
    if (lengths[256] == 0)
        return PW_INFLATE_ERR_DATA;
    err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return PW_INFLATE_ERR_DATA;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return PW_INFLATE_ERR_DATA;
    return codes(s, &lencode, &distcode);
}

int pw_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
               uint32_t *out_len)
{
    State s;
    int last, err;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.src_len = src_len;
    s.dst = dst;
    s.dst_cap = dst_cap;
    do {
        uint32_t type;
        last = (int)bits(&s, 1);
        type = bits(&s, 2);
        if (s.err) {
            err = s.err;
            break;
        }
        if (type == 0)
            err = stored(&s);
        else if (type == 1)
            err = fixed(&s);
        else if (type == 2)
            err = dynamic(&s);
        else
            err = PW_INFLATE_ERR_DATA;
        if (err == PW_INFLATE_OK && s.err)
            err = s.err;
    } while (!last && err == PW_INFLATE_OK);
    if (out_len)
        *out_len = s.dst_pos;
    return err;
}
