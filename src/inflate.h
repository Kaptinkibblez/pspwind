/*
 * inflate.h - raw DEFLATE (RFC 1951) decoder for the cooked text blocks.
 *
 * Small and allocation free: two Huffman tables on the stack, output into a
 * caller buffer whose size is known from the pack directory. Every read is
 * bounds checked, so a corrupt block fails cleanly.
 */
#ifndef PSPWIND_INFLATE_H
#define PSPWIND_INFLATE_H

#include "common.h"

enum {
    PW_INFLATE_OK = 0,
    PW_INFLATE_ERR_INPUT = -1,    /* ran out of input */
    PW_INFLATE_ERR_OUTPUT = -2,   /* output buffer too small */
    PW_INFLATE_ERR_DATA = -3      /* malformed stream */
};

/* Decodes src into dst; *out_len receives the decoded size. */
int pw_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
               uint32_t *out_len);

#endif /* PSPWIND_INFLATE_H */
