/* zlib (RFC 1950) / DEFLATE (RFC 1951) decompression. Virtools files compress their header and data
   sections with zlib (CKZlib.dll); the InstallShield cabinets on the CD use raw DEFLATE. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decompress a zlib stream into dst (capacity cap). Returns the decompressed size, or -1 on a corrupt
   stream, a checksum mismatch or when the output doesn't fit. */
long zlib_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
/* Same for a raw DEFLATE stream (no header, no checksum). *consumed (may be NULL) = input bytes used. */
long deflate_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *consumed);
/* A raw DEFLATE chunk that ends with a sync flush rather than a final block (InstallShield cabinets
   store a file as a series of such chunks): blocks are decoded until a final block or until the
   input ends on a block boundary. Returns the decompressed size or -1. */
long deflate_inflate_flushed(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
