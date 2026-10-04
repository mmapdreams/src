/*
 * xxh3_64_mini.h -- minimal scalar XXH3-64 for BFS.
 *
 * Declares a single function: compute the 64-bit XXH3 of `len` bytes
 * at `data` with default secret and seed=0.
 *
 * Implemented in xxh3_64_mini.c with no SIMD or libc dependencies, so
 * it compiles identically in userland and inside the OpenBSD kernel.
 * The few required fixed-width types and `size_t` come from
 * <sys/types.h> under _KERNEL and from <stddef.h>/<stdint.h> otherwise.
 * Output is bit-identical to upstream xxHash v0.8.2 XXH3_64bits(data,
 * len) for len >= 1; cross-checked by tests/xxh3_64_mini_tests.c.
 *
 * License: upstream xxHash is BSD-2-Clause (Copyright 2012-2021 Yann
 * Collet). This file is a reimplementation that copies the XXH3 public
 * secret table and primes verbatim; all code is derivative of the
 * upstream scalar path and retains the same license terms.
 */
#ifndef BFS_VENDOR_XXH3_64_MINI_H
#define BFS_VENDOR_XXH3_64_MINI_H

#if defined(_KERNEL) || defined(_STANDALONE)
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint64_t bfs_xxh3_64(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BFS_VENDOR_XXH3_64_MINI_H */
