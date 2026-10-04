/* SPDX-License-Identifier: ISC
 * branch_fs_codec.h -- branch filesystem identity and namespace record framing.
 */
#ifndef CFS_BRANCH_FS_CODEC_H
#define CFS_BRANCH_FS_CODEC_H

#include "branch_store.h"

#define CFS_BF_ROOT_BYTES 32u
#define CFS_BF_INODE_BYTES 128u
#define CFS_BF_ID_BYTES 16u
#define CFS_BF_NAME_MAX 255u
#define CFS_BF_SYMLINK_MAX 4096u

enum { CFS_BF_REGULAR = 1, CFS_BF_DIRECTORY = 2, CFS_BF_SYMLINK = 3 };

typedef struct cfs_bf_id {
    uint64_t birth;
    uint64_t ordinal;
} cfs_bf_id_t;

typedef struct cfs_bf_time {
    int64_t seconds;
    uint32_t nanoseconds;
} cfs_bf_time_t;

typedef struct cfs_bf_inode {
    cfs_bf_id_t parent;
    cfs_bf_time_t atime;
    cfs_bf_time_t mtime;
    cfs_bf_time_t ctime;
    cfs_bf_time_t btime;
    uint64_t links;
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint16_t type;
    uint16_t mode;
} cfs_bf_inode_t;

int cfs_bf_id_valid(cfs_bf_id_t id);
int cfs_bf_id_equal(cfs_bf_id_t left, cfs_bf_id_t right);
int cfs_bf_name_valid(const void *name, uint16_t length);

/* Codecs require disjoint inputs and outputs; all refusals preserve outputs. */
int cfs_bf_id_key(cfs_bf_id_t id, uint8_t *out);
int cfs_bf_dirent_frame(cfs_bf_id_t id, uint8_t *out);
int cfs_bf_dirent_decode(const void *bytes, uint32_t length, cfs_bf_id_t *out);
int cfs_bf_inode_frame(cfs_bf_inode_t inode, int has_tree, uint8_t *out);
int cfs_bf_inode_decode(const void *bytes, uint32_t length, int has_tree,
    cfs_bf_inode_t *out);
int cfs_bf_root_frame(cfs_bf_id_t root, int has_orphans, uint8_t *out);
int cfs_bf_root_decode(const void *bytes, uint32_t length, uint32_t children,
    cfs_bf_id_t *out_root);

#endif /* CFS_BRANCH_FS_CODEC_H */
