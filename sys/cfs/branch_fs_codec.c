/* SPDX-License-Identifier: ISC
 * branch_fs_codec.c -- canonical namespace bytes and local field validation.
 */
#include "branch_fs_codec.h"

#include <string.h>

#include "codec.h"

#define ROOT_MAGIC UINT64_C(0x3230305346534643)

int cfs_bf_id_valid(cfs_bf_id_t id) {
    return id.birth != 0u && id.ordinal < 128u;
}

int cfs_bf_id_equal(cfs_bf_id_t left, cfs_bf_id_t right) {
    return left.birth == right.birth && left.ordinal == right.ordinal;
}

int cfs_bf_name_valid(const void *name, uint16_t length) {
    const uint8_t *bytes = name;
    if (name == NULL || length == 0u || length > CFS_BF_NAME_MAX ||
        (length == 1u && bytes[0] == '.') ||
        (length == 2u && bytes[0] == '.' && bytes[1] == '.')) {
        return 0;
    }

    for (uint16_t i = 0u; i < length; i++) {
        if (bytes[i] == 0u || bytes[i] == '/') {
            return 0;
        }
    }

    return 1;
}

static void put_id(uint8_t *bytes, cfs_bf_id_t id) {
    cfs_put64(bytes, id.birth);
    cfs_put64(bytes + 8u, id.ordinal);
}

static cfs_bf_id_t get_id(const uint8_t *bytes) {
    return (cfs_bf_id_t){cfs_get64(bytes), cfs_get64(bytes + 8u)};
}

int cfs_bf_id_key(cfs_bf_id_t id, uint8_t *out) {
    if (!cfs_bf_id_valid(id) || out == NULL) {
        return CFS_BS_EINVAL;
    }

    for (uint32_t i = 0u; i < 8u; i++) {
        out[i] = (uint8_t)(id.birth >> (56u - i * 8u));
        out[8u + i] = (uint8_t)(id.ordinal >> (56u - i * 8u));
    }

    return CFS_BS_OK;
}

int cfs_bf_dirent_frame(cfs_bf_id_t id, uint8_t *out) {
    if (!cfs_bf_id_valid(id) || out == NULL) {
        return CFS_BS_EINVAL;
    }

    put_id(out, id);
    return CFS_BS_OK;
}

int cfs_bf_dirent_decode(const void *bytes, uint32_t length, cfs_bf_id_t *out) {
    if (bytes == NULL || out == NULL) {
        return CFS_BS_EINVAL;
    }

    if (length != CFS_BF_ID_BYTES) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bf_id_t id = get_id(bytes);
    if (!cfs_bf_id_valid(id)) {
        return CFS_BS_ECORRUPT;
    }

    *out = id;
    return CFS_BS_OK;
}

static int inode_valid(cfs_bf_inode_t inode, int has_tree) {
    if ((has_tree != 0 && has_tree != 1) || inode.mode > 07777u ||
        inode.size > INT64_MAX ||
        inode.atime.nanoseconds >= 1000000000u ||
        inode.mtime.nanoseconds >= 1000000000u ||
        inode.ctime.nanoseconds >= 1000000000u ||
        inode.btime.nanoseconds >= 1000000000u ||
        (inode.size == 0u && has_tree != 0)) {
        return 0;
    }

    if (inode.type == CFS_BF_DIRECTORY) {
        return cfs_bf_id_valid(inode.parent) &&
            ((inode.size != 0u) == (has_tree != 0)) &&
            ((inode.links >= 2u && inode.links <= inode.size + 2u) ||
                (inode.links == 0u && inode.size == 0u));
    }

    return (inode.type == CFS_BF_REGULAR ||
        (inode.type == CFS_BF_SYMLINK && inode.size >= 1u &&
            inode.size <= CFS_BF_SYMLINK_MAX && has_tree)) &&
        inode.parent.birth == 0u && inode.parent.ordinal == 0u;
}

static void put_time(uint8_t *bytes, cfs_bf_time_t time) {
    cfs_put64(bytes, (uint64_t)time.seconds);
    cfs_put32(bytes + 8u, time.nanoseconds);
}

static cfs_bf_time_t get_time(const uint8_t *bytes) {
    uint64_t seconds = cfs_get64(bytes);
    cfs_bf_time_t time = {
        .seconds = seconds <= INT64_MAX ? (int64_t)seconds :
            -1 - (int64_t)(UINT64_MAX - seconds),
        .nanoseconds = cfs_get32(bytes + 8u)
    };
    return time;
}

int cfs_bf_inode_frame(cfs_bf_inode_t inode, int has_tree, uint8_t *out) {
    if (out == NULL || !inode_valid(inode, has_tree)) {
        return CFS_BS_EINVAL;
    }

    memset(out, 0, CFS_BF_INODE_BYTES);
    cfs_put16(out, inode.type);
    cfs_put16(out + 2u, inode.mode);
    cfs_put32(out + 8u, inode.uid);
    cfs_put32(out + 12u, inode.gid);
    cfs_put64(out + 16u, inode.links);
    cfs_put64(out + 24u, inode.size);
    put_id(out + 32u, inode.parent);
    put_time(out + 56u, inode.atime);
    put_time(out + 72u, inode.mtime);
    put_time(out + 88u, inode.ctime);
    put_time(out + 104u, inode.btime);
    return CFS_BS_OK;
}

int cfs_bf_inode_decode(const void *bytes, uint32_t length, int has_tree,
    cfs_bf_inode_t *out) {
    const uint8_t *data = bytes;
    if (bytes == NULL || out == NULL || (has_tree != 0 && has_tree != 1)) {
        return CFS_BS_EINVAL;
    }

    if (length != CFS_BF_INODE_BYTES || cfs_get32(data + 4u) != 0u ||
        cfs_get64(data + 48u) != 0u || cfs_get64(data + 120u) != 0u) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < 4u; i++) {
        if (cfs_get32(data + 68u + i * 16u) != 0u) {
            return CFS_BS_ECORRUPT;
        }
    }

    cfs_bf_inode_t inode = {
        .type = cfs_get16(data), .mode = cfs_get16(data + 2u),
        .uid = cfs_get32(data + 8u), .gid = cfs_get32(data + 12u),
        .links = cfs_get64(data + 16u), .size = cfs_get64(data + 24u),
        .parent = get_id(data + 32u), .atime = get_time(data + 56u),
        .mtime = get_time(data + 72u), .ctime = get_time(data + 88u),
        .btime = get_time(data + 104u)
    };
    if (!inode_valid(inode, has_tree)) {
        return CFS_BS_ECORRUPT;
    }

    *out = inode;
    return CFS_BS_OK;
}

int cfs_bf_root_frame(cfs_bf_id_t root, int has_orphans, uint8_t *out) {
    if (!cfs_bf_id_valid(root) || (has_orphans != 0 && has_orphans != 1) ||
        out == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_put64(out, ROOT_MAGIC);
    cfs_put64(out + 8u, (uint64_t)has_orphans);
    put_id(out + 16u, root);
    return CFS_BS_OK;
}

int cfs_bf_root_decode(const void *bytes, uint32_t length, uint32_t children,
    cfs_bf_id_t *out_root) {
    const uint8_t *data = bytes;
    if (bytes == NULL || out_root == NULL) {
        return CFS_BS_EINVAL;
    }

    if (length != CFS_BF_ROOT_BYTES || cfs_get64(data) != ROOT_MAGIC ||
        cfs_get64(data + 8u) > 1u || children != 1u + cfs_get64(data + 8u)) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bf_id_t root = get_id(data + 16u);
    if (!cfs_bf_id_valid(root)) {
        return CFS_BS_ECORRUPT;
    }

    *out_root = root;
    return CFS_BS_OK;
}
