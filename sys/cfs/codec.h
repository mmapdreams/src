/* SPDX-License-Identifier: ISC
 *
 * codec.h -- on-disk scalar codec: little-endian byte accessors and
 * the 64-bit block hash.
 *
 * Every on-disk integer in cfs is little-endian at a byte-defined
 * offset. These helpers are the only sanctioned way to read or write
 * them, so unaligned access and host endianness never leak into the
 * format.
 */
#ifndef CFS_CODEC_H
#define CFS_CODEC_H

#include <stddef.h>
#include <stdint.h>

uint16_t cfs_get16(const uint8_t *src);
uint32_t cfs_get32(const uint8_t *src);
uint64_t cfs_get64(const uint8_t *src);

void cfs_put16(uint8_t *dst, uint16_t v);
void cfs_put32(uint8_t *dst, uint32_t v);
void cfs_put64(uint8_t *dst, uint64_t v);

/* Big-endian accessors for the sole exception to the little-endian rule: item
 * key integers, stored MSB-first so memcmp is the key comparator. */
uint64_t cfs_get64be(const uint8_t *src);
void cfs_put64be(uint8_t *dst, uint64_t v);

/* XXH3-64, default secret and seed 0: the on-disk checksum for every
 * checksummed structure. Scalar-only, so kernel and userland agree exactly. */
uint64_t cfs_hash64(const void *data, size_t len);

#endif /* CFS_CODEC_H */
