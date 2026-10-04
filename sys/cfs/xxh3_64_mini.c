/*
 * xxh3_64_mini.c -- minimal scalar XXH3-64 for BFS.
 *
 * A self-contained reimplementation of upstream xxHash v0.8.2
 * XXH3_64bits(data, len), covering only the scalar code path with the
 * default 192-byte secret and seed=0.
 *
 * Why a separate mini copy instead of vendor/xxhash/xxhash.h:
 *   - xxhash.h is ~6700 lines and includes <string.h>, <limits.h>, and
 *     SIMD intrinsic headers (emmintrin.h, arm_neon.h) unconditionally
 *     whenever the target-feature macros are set. Those userland
 *     headers redeclare malloc/free/memcpy and collide with the
 *     OpenBSD kernel's own declarations (see sys/malloc.h,
 *     sys/systm.h) when the file is compiled with -D_KERNEL.
 *   - The kernel cannot use SIMD anyway: FPU/vector state is not saved
 *     across context switches, so SSE2/NEON in-kernel would corrupt
 *     userspace vector registers. A scalar-only path is all a kernel
 *     build could ever use.
 *   - Keeping a single scalar implementation for userland and kernel
 *     keeps block checksums byte-identical across the two, which is a
 *     correctness requirement since both paths read and write the same
 *     on-disk images.
 *
 * Cross-checked for bit-identical output against upstream
 * XXH3_64bits(data, len) by tests/xxh3_64_mini_tests.c.
 *
 * License: BSD-2-Clause, derived from upstream xxHash by Yann Collet.
 * See vendor/xxhash/xxhash.h for the upstream license text.
 */

#include "xxh3_64_mini.h"
#include "bfs_macros.h"

/*
 * Block checksums hash caller-owned, thread-private buffers, so the
 * byte-wise reads here race with nothing. Exclude the whole unit from
 * ThreadSanitizer data-access instrumentation: each 8-byte little-endian
 * read otherwise expands to eight __tsan_read1 calls that dominate tsan
 * suite runtime, and the combined ubsan+tsan build keeps these readers
 * out-of-line so the per-byte calls survive at -O2. happens-before
 * tracking at call boundaries is kept.
 */
BFS_NO_SANITIZE_THREAD_BEGIN

/* -- Primes and constants copied verbatim from upstream xxHash ------- */

#define XXH_PRIME32_1  0x9E3779B1U
#define XXH_PRIME32_2  0x85EBCA77U
#define XXH_PRIME32_3  0xC2B2AE3DU
#define XXH_PRIME64_1  0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2  0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3  0x165667B19E3779F9ULL
#define XXH_PRIME64_4  0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5  0x27D4EB2F165667C5ULL
#define XXH_PRIME_MX1  0x165667919E3779F9ULL
#define XXH_PRIME_MX2  0x9FB21C651E98DF25ULL

#define XXH3_SECRET_SIZE        192
#define XXH3_STRIPE_LEN          64
#define XXH3_ACC_NB               8
#define XXH3_SECRET_CONSUME_RATE  8
#define XXH3_MIDSIZE_MAX        240
#define XXH3_MIDSIZE_STARTOFFSET  3
#define XXH3_MIDSIZE_LASTOFFSET  17
#define XXH3_SECRET_LASTACC_START  7
#define XXH3_SECRET_MERGEACCS_START 11

static const unsigned char xxh3_k_secret[XXH3_SECRET_SIZE] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe,
    0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb,
    0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78,
    0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e,
    0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
    0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e,
    0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f,
    0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31,
    0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3,
    0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49,
    0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc,
    0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28,
    0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

/* -- Small helpers (no <string.h>, no intrinsics) --------------------- */

static uint32_t xxh3_read_le32(const unsigned char *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t xxh3_read_le64(const unsigned char *p) {
    return (uint64_t)p[0]
        | ((uint64_t)p[1] << 8)
        | ((uint64_t)p[2] << 16)
        | ((uint64_t)p[3] << 24)
        | ((uint64_t)p[4] << 32)
        | ((uint64_t)p[5] << 40)
        | ((uint64_t)p[6] << 48)
        | ((uint64_t)p[7] << 56);
}

/*
 * Portable unsigned 64x64 -> 128 multiply, returning lo ^ hi.
 *
 * Uses 32-bit partial products so the result is bit-identical across
 * ILP32/LP64 hosts; no reliance on __int128 or MSVC intrinsics. This
 * is the XXH3_mul128_fold64 operation from upstream.
 */
static uint64_t xxh3_mul128_fold64(uint64_t a, uint64_t b) {
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;
    uint64_t ll = a_lo * b_lo;
    uint64_t lh = a_lo * b_hi;
    uint64_t hl = a_hi * b_lo;
    uint64_t hh = a_hi * b_hi;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    uint64_t lo = ((uint32_t)ll) | (mid << 32);
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);

    return lo ^ hi;
}

static uint64_t xxh3_xorshift64(uint64_t v, int shift) {
    return v ^ (v >> shift);
}

static uint64_t xxh3_avalanche(uint64_t h) {
    h = xxh3_xorshift64(h, 37);
    h *= XXH_PRIME_MX1;
    h = xxh3_xorshift64(h, 32);
    return h;
}

/*
 * XXH64 avalanche (distinct from XXH3_avalanche above). Used by the
 * XXH3-64 len=0..3 and len==0 code paths.
 */
static uint64_t xxh64_avalanche(uint64_t h) {
    h ^= h >> 33;
    h *= XXH_PRIME64_2;
    h ^= h >> 29;
    h *= XXH_PRIME64_3;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh3_swap64(uint64_t v) {
    return ((v & 0x00000000000000ffULL) << 56)
         | ((v & 0x000000000000ff00ULL) << 40)
         | ((v & 0x0000000000ff0000ULL) << 24)
         | ((v & 0x00000000ff000000ULL) << 8)
         | ((v & 0x000000ff00000000ULL) >> 8)
         | ((v & 0x0000ff0000000000ULL) >> 24)
         | ((v & 0x00ff000000000000ULL) >> 40)
         | ((v & 0xff00000000000000ULL) >> 56);
}

static uint64_t xxh3_rrmxmx(uint64_t h, uint64_t len) {
    /* 64-bit rotate left by 49 and 24. */
    h ^= ((h << 49) | (h >> 15)) ^ ((h << 24) | (h >> 40));
    h *= XXH_PRIME_MX2;
    h ^= (h >> 35) + len;
    h *= XXH_PRIME_MX2;
    return xxh3_xorshift64(h, 28);
}

/* -- Small-input dispatch (len <= 240) -------------------------------- */

static uint64_t xxh3_len_1to3(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    unsigned char c1 = input[0];
    unsigned char c2 = input[len >> 1];
    unsigned char c3 = input[len - 1];
    uint32_t combined = ((uint32_t)c1 << 16) | ((uint32_t)c2 << 24)
        | ((uint32_t)c3) | ((uint32_t)len << 8);
    uint64_t bitflip = ((uint64_t)xxh3_read_le32(secret)
        ^ (uint64_t)xxh3_read_le32(secret + 4));
    uint64_t keyed = (uint64_t)combined ^ bitflip;

    return xxh64_avalanche(keyed);
}

static uint64_t xxh3_len_4to8(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    uint32_t input1 = xxh3_read_le32(input);
    uint32_t input2 = xxh3_read_le32(input + len - 4);
    uint64_t bitflip = xxh3_read_le64(secret + 8) ^ xxh3_read_le64(secret + 16);
    uint64_t input64 = (uint64_t)input2 + (((uint64_t)input1) << 32);
    uint64_t keyed = input64 ^ bitflip;

    return xxh3_rrmxmx(keyed, len);
}

static uint64_t xxh3_len_9to16(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    uint64_t bitflip1 = xxh3_read_le64(secret + 24) ^ xxh3_read_le64(secret + 32);
    uint64_t bitflip2 = xxh3_read_le64(secret + 40) ^ xxh3_read_le64(secret + 48);
    uint64_t input_lo = xxh3_read_le64(input) ^ bitflip1;
    uint64_t input_hi = xxh3_read_le64(input + len - 8) ^ bitflip2;
    uint64_t acc = (uint64_t)len
        + xxh3_swap64(input_lo)
        + input_hi
        + xxh3_mul128_fold64(input_lo, input_hi);

    return xxh3_avalanche(acc);
}

static uint64_t xxh3_len_0to16(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    if (len > 8) {
        return xxh3_len_9to16(input, len, secret);
    }

    if (len >= 4) {
        return xxh3_len_4to8(input, len, secret);
    }

    if (len != 0) {
        return xxh3_len_1to3(input, len, secret);
    }

    /* len == 0: upstream returns XXH64_avalanche of secret bits. */
    return xxh64_avalanche(
        xxh3_read_le64(secret + 56) ^ xxh3_read_le64(secret + 64));
}

static uint64_t xxh3_mix16b(const unsigned char *input,
    const unsigned char *secret) {
    uint64_t input_lo = xxh3_read_le64(input);
    uint64_t input_hi = xxh3_read_le64(input + 8);

    return xxh3_mul128_fold64(
        input_lo ^ xxh3_read_le64(secret),
        input_hi ^ xxh3_read_le64(secret + 8));
}

static uint64_t xxh3_len_17to128(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    uint64_t acc = (uint64_t)len * XXH_PRIME64_1;

    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc += xxh3_mix16b(input + 48, secret + 96);
                acc += xxh3_mix16b(input + len - 64, secret + 112);
            }

            acc += xxh3_mix16b(input + 32, secret + 64);
            acc += xxh3_mix16b(input + len - 48, secret + 80);
        }

        acc += xxh3_mix16b(input + 16, secret + 32);
        acc += xxh3_mix16b(input + len - 32, secret + 48);
    }

    acc += xxh3_mix16b(input, secret);
    acc += xxh3_mix16b(input + len - 16, secret + 16);
    return xxh3_avalanche(acc);
}

static uint64_t xxh3_len_129to240(const unsigned char *input, size_t len,
    const unsigned char *secret) {
    /*
     * XXH3_SECRET_SIZE_MIN (136) is the offset base used by upstream
     * for the "last bytes" mix in this branch, not XXH3_SECRET_SIZE
     * (192). The 136 value is intrinsic to the algorithm; the default
     * secret table is larger than the minimum size used here.
     */
    uint64_t acc = (uint64_t)len * XXH_PRIME64_1;
    uint64_t acc_end;
    size_t nb_rounds = len / 16;
    size_t i;

    for (i = 0; i < 8; i++) {
        acc += xxh3_mix16b(input + 16 * i, secret + 16 * i);
    }
    acc = xxh3_avalanche(acc);
    acc_end = xxh3_mix16b(input + len - 16,
        secret + 136 - XXH3_MIDSIZE_LASTOFFSET);
    for (i = 8; i < nb_rounds; i++) {
        acc_end += xxh3_mix16b(input + 16 * i,
            secret + 16 * (i - 8) + XXH3_MIDSIZE_STARTOFFSET);
    }
    return xxh3_avalanche(acc + acc_end);
}

/* -- Long-input path (len > 240) -------------------------------------- */

static void xxh3_accumulate_512(uint64_t acc[XXH3_ACC_NB],
    const unsigned char *input, const unsigned char *secret) {
    size_t lane;

    for (lane = 0; lane < XXH3_ACC_NB; lane++) {
        uint64_t data_val = xxh3_read_le64(input + lane * 8);
        uint64_t data_key = data_val ^ xxh3_read_le64(secret + lane * 8);

        acc[lane ^ 1] += data_val; /* swap adjacent lanes */
        acc[lane] += (uint64_t)(uint32_t)data_key
            * (uint64_t)(uint32_t)(data_key >> 32);
    }
}

static void xxh3_scramble_acc(uint64_t acc[XXH3_ACC_NB],
    const unsigned char *secret) {
    size_t lane;

    for (lane = 0; lane < XXH3_ACC_NB; lane++) {
        uint64_t key64 = xxh3_read_le64(secret + lane * 8);
        uint64_t v = acc[lane];

        v = xxh3_xorshift64(v, 47);
        v ^= key64;
        v *= XXH_PRIME32_1;
        acc[lane] = v;
    }
}

static void xxh3_accumulate(uint64_t acc[XXH3_ACC_NB],
    const unsigned char *input, const unsigned char *secret,
    size_t nb_stripes) {
    size_t i;

    for (i = 0; i < nb_stripes; i++) {
        xxh3_accumulate_512(acc, input + i * XXH3_STRIPE_LEN,
            secret + i * XXH3_SECRET_CONSUME_RATE);
    }
}

static uint64_t xxh3_mix_2accs(const uint64_t *acc,
    const unsigned char *secret) {
    return xxh3_mul128_fold64(
        acc[0] ^ xxh3_read_le64(secret),
        acc[1] ^ xxh3_read_le64(secret + 8));
}

static uint64_t xxh3_merge_accs(const uint64_t acc[XXH3_ACC_NB],
    const unsigned char *secret, uint64_t start) {
    uint64_t result = start;
    size_t i;

    for (i = 0; i < 4; i++) {
        result += xxh3_mix_2accs(acc + 2 * i, secret + 16 * i);
    }
    return xxh3_avalanche(result);
}

static uint64_t xxh3_hash_long(const unsigned char *input, size_t len) {
    /*
     * Upstream XXH3_INIT_ACC:
     *   { PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3,
     *     PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1 }
     */
    uint64_t acc[XXH3_ACC_NB] = {
        XXH_PRIME32_3, XXH_PRIME64_1, XXH_PRIME64_2, XXH_PRIME64_3,
        XXH_PRIME64_4, XXH_PRIME32_2, XXH_PRIME64_5, XXH_PRIME32_1,
    };
    const unsigned char *secret = xxh3_k_secret;
    size_t secret_size = XXH3_SECRET_SIZE;
    size_t nb_stripes_per_block =
        (secret_size - XXH3_STRIPE_LEN) / XXH3_SECRET_CONSUME_RATE;
    size_t block_len = XXH3_STRIPE_LEN * nb_stripes_per_block;
    size_t nb_blocks = (len - 1) / block_len;
    size_t n;
    size_t nb_stripes;
    const unsigned char *last_stripe;

    for (n = 0; n < nb_blocks; n++) {
        xxh3_accumulate(acc, input + n * block_len, secret,
            nb_stripes_per_block);
        xxh3_scramble_acc(acc, secret + secret_size - XXH3_STRIPE_LEN);
    }

    /* last partial block */
    nb_stripes = ((len - 1) - (block_len * nb_blocks)) / XXH3_STRIPE_LEN;
    xxh3_accumulate(acc, input + nb_blocks * block_len, secret, nb_stripes);

    /* last (possibly overlapping) stripe, hashed with a shifted secret */
    last_stripe = input + len - XXH3_STRIPE_LEN;
    xxh3_accumulate_512(acc, last_stripe,
        secret + secret_size - XXH3_STRIPE_LEN - XXH3_SECRET_LASTACC_START);

    return xxh3_merge_accs(acc,
        secret + XXH3_SECRET_MERGEACCS_START,
        (uint64_t)len * XXH_PRIME64_1);
}

uint64_t bfs_xxh3_64(const void *data, size_t len) {
    const unsigned char *input = (const unsigned char *)data;
    const unsigned char *secret = xxh3_k_secret;

    if (len <= 16) {
        return xxh3_len_0to16(input, len, secret);
    }

    if (len <= 128) {
        return xxh3_len_17to128(input, len, secret);
    }

    if (len <= XXH3_MIDSIZE_MAX) {
        return xxh3_len_129to240(input, len, secret);
    }

    return xxh3_hash_long(input, len);
}

BFS_NO_SANITIZE_THREAD_END
