// Written by Alun Morris and Claude Code
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LZ4_decompress_safe - Decompress an LZ4 block.
 *
 * src              : pointer to the compressed LZ4 block data
 * dst              : pointer to the output buffer
 * compressedSize   : exact byte length of the compressed block
 * maxDecompressedSize : size of the dst buffer (safety limit)
 *
 * Returns the number of bytes written to dst on success, or a negative value
 * on error (output overrun, input truncated, corrupted data).
 *
 * This implements the LZ4 *block* format (not the LZ4 frame format).
 *
 * Wire format used by the Python preprocessor:
 *   bytes 0-3  : original (uncompressed) size as uint32 little-endian
 *   bytes 4..  : raw LZ4 block data
 *
 * Caller should therefore pass (blob+4) as src and (blob_len-4) as
 * compressedSize, having read the 4-byte size prefix separately.
 */
int LZ4_decompress_safe(const char *src, char *dst,
                        int compressedSize, int maxDecompressedSize);

#ifdef __cplusplus
}
#endif
