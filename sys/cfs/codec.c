/* SPDX-License-Identifier: ISC
 *
 * codec.c -- little-endian byte accessors and block hash.
 */
#include "codec.h"

#include "xxh3_64_mini.h"

uint16_t cfs_get16(const uint8_t *src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

uint32_t cfs_get32(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

uint64_t cfs_get64(const uint8_t *src) {
    return (uint64_t)cfs_get32(src) | ((uint64_t)cfs_get32(src + 4) << 32);
}

void cfs_put16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)(v >> 8);
}

void cfs_put32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
    dst[2] = (uint8_t)((v >> 16) & 0xff);
    dst[3] = (uint8_t)((v >> 24) & 0xff);
}

void cfs_put64(uint8_t *dst, uint64_t v) {
    cfs_put32(dst, (uint32_t)(v & 0xffffffffu));
    cfs_put32(dst + 4, (uint32_t)(v >> 32));
}

uint64_t cfs_get64be(const uint8_t *src) {
    return ((uint64_t)src[0] << 56) | ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) | ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8) | (uint64_t)src[7];
}

void cfs_put64be(uint8_t *dst, uint64_t v) {
    dst[0] = (uint8_t)(v >> 56);
    dst[1] = (uint8_t)((v >> 48) & 0xff);
    dst[2] = (uint8_t)((v >> 40) & 0xff);
    dst[3] = (uint8_t)((v >> 32) & 0xff);
    dst[4] = (uint8_t)((v >> 24) & 0xff);
    dst[5] = (uint8_t)((v >> 16) & 0xff);
    dst[6] = (uint8_t)((v >> 8) & 0xff);
    dst[7] = (uint8_t)(v & 0xff);
}

uint64_t cfs_hash64(const void *data, size_t len) {
    return bfs_xxh3_64(data, len);
}
