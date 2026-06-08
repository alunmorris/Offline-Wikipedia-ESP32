// Written by Alun Morris and Claude Code
/*
 * Minimal LZ4 block decompressor.
 *
 * Implements the LZ4 block format as specified at:
 *   https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * This is NOT the LZ4 frame format — it handles only raw LZ4 blocks, which is
 * what Python's lz4.block.compress() produces (with store_size=True the first
 * 4 bytes are the original size; the block data starts at byte 4).
 *
 * Block format summary:
 *   A block is a sequence of sequences. Each sequence:
 *     1. token byte: high nibble = literal_length, low nibble = match_length-4
 *     2. If literal_length == 15: read extra bytes (each 0xFF adds 255) until
 *        a byte < 255; total literal_length += those bytes.
 *     3. literal_length bytes of literal data copied verbatim to output.
 *     4. (omitted for the very last sequence) 2-byte LE match offset.
 *     5. If low_nibble == 15: read extra bytes (same scheme) for match_length;
 *        total match_length = 4 + low_nibble + extra.
 *     6. Copy match_length bytes from (output_ptr - offset) to output_ptr.
 *        The source and destination may overlap (sliding window copy).
 *
 *   The last sequence in the block ends after the literals — there is no
 *   match offset or match copy for it.
 */

#include "lz4.h"
#include <stdint.h>
#include <string.h>

/* Read a 16-bit little-endian value from an unaligned pointer. */
static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int LZ4_decompress_safe(const char *src, char *dst,
                        int compressedSize, int maxDecompressedSize)
{
    const uint8_t *ip      = (const uint8_t *)src;          /* input pointer  */
    const uint8_t *ip_end  = ip + compressedSize;
    uint8_t       *op      = (uint8_t *)dst;                /* output pointer */
    uint8_t       *op_end  = op + maxDecompressedSize;
    uint8_t       *op_start = op;

    if (compressedSize <= 0 || maxDecompressedSize <= 0) return -1;

    while (ip < ip_end) {

        /* ---- 1. Read token ---- */
        if (ip >= ip_end) goto _output_error;
        uint8_t token = *ip++;

        uint32_t literal_length = (token >> 4) & 0x0F;
        uint32_t match_length   = (token     ) & 0x0F;

        /* ---- 2. Extra literal length bytes ---- */
        if (literal_length == 15) {
            uint8_t s;
            do {
                if (ip >= ip_end) goto _output_error;
                s = *ip++;
                literal_length += s;
            } while (s == 255);
        }

        /* ---- 3. Copy literals ---- */
        if (literal_length > 0) {
            if (ip + literal_length > ip_end)  goto _output_error;
            if (op + literal_length > op_end)  goto _output_error;
            memcpy(op, ip, literal_length);
            ip += literal_length;
            op += literal_length;
        }

        /* ---- End of block: last sequence has no match part ---- */
        if (ip >= ip_end) break;

        /* ---- 4. Match offset (2 bytes LE) ---- */
        if (ip + 2 > ip_end) goto _output_error;
        uint16_t offset = read_u16_le(ip);
        ip += 2;

        if (offset == 0) goto _output_error;            /* invalid per spec */

        uint8_t *match = op - offset;
        if (match < op_start) goto _output_error;       /* before output start */

        /* ---- 5. Extra match length bytes ---- */
        match_length += 4;   /* minimum match length is 4 */
        if ((token & 0x0F) == 15) {
            uint8_t s;
            do {
                if (ip >= ip_end) goto _output_error;
                s = *ip++;
                match_length += s;
            } while (s == 255);
        }

        /* ---- 6. Copy match (may overlap — copy byte by byte) ---- */
        if (op + match_length > op_end) goto _output_error;

        /*
         * The match window can overlap with the output being written (e.g.
         * offset=1 means repeat the last byte). We must copy byte by byte so
         * that newly written bytes are visible to subsequent reads within the
         * same match, which memcpy() does not guarantee for overlapping ranges.
         */
        uint8_t *match_end = match + match_length;
        if (match + match_length <= op) {
            /* No overlap: safe to use memcpy for speed. */
            memcpy(op, match, match_length);
            op += match_length;
        } else {
            /* Overlap (offset < match_length): copy byte by byte. */
            (void)match_end;
            for (uint32_t i = 0; i < match_length; i++) {
                *op++ = *(match + i);
            }
        }
    }

    return (int)(op - op_start);

_output_error:
    return -1;
}
