/* SPDX-License-Identifier: ISC
 * branch_change.c -- canonical framing, bounded decoding, and EOF receipts.
 */
#include "branch_change_internal.h"
#include "codec.h"

#include <string.h>

enum { NEED_HEADER = 1, CHANGES = 2, ENDED = 3, FINISHED = 4 };

typedef struct token {
    const uint8_t *bytes;
    uint32_t length;
} token_t;

static int token_equal(token_t token, const char *text, uint32_t length) {
    return length == token.length && memcmp(token.bytes, text, length) == 0;
}

static token_t token_field(token_t token, const char *prefix, uint32_t length) {
    if (length >= token.length || memcmp(token.bytes, prefix, length) != 0) {
        return (token_t){NULL, 0u};
    }

    return (token_t){token.bytes + length, token.length - length};
}

/* Every grammar tag is a literal; its bound does not require hosted strings. */
#define TOKEN_EQUAL(token, text) token_equal((token), (text), sizeof(text) - 1u)
#define TOKEN_FIELD(token, text) token_field((token), (text), sizeof(text) - 1u)

static int number(token_t token, uint64_t *out) {
    uint64_t value = 0u;
    if (token.length == 0u || (token.length > 1u && token.bytes[0] == '0')) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < token.length; i++) {
        uint8_t c = token.bytes[i];
        if (c < '0' || c > '9' || value > (UINT64_MAX - (c - '0')) / 10u) {
            return CFS_BS_ECORRUPT;
        }

        value = value * 10u + (c - '0');
    }

    *out = value;
    return CFS_BS_OK;
}

static int nibble(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int unhex(token_t token, uint8_t *bytes, uint32_t capacity,
    uint32_t *out_length) {
    if (token.length == 0u || token.length % 2u != 0u ||
        token.length / 2u > capacity) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < token.length / 2u; i++) {
        int high = nibble(token.bytes[2u * i]);
        int low = nibble(token.bytes[2u * i + 1u]);
        if (high < 0 || low < 0) {
            return CFS_BS_ECORRUPT;
        }

        bytes[i] = (uint8_t)(high * 16 + low);
    }

    *out_length = token.length / 2u;
    return CFS_BS_OK;
}

static int fixed_hex(token_t token, uint8_t *bytes, uint32_t length) {
    uint32_t decoded;
    if (token.length != length * 2u) {
        return CFS_BS_ECORRUPT;
    }

    return unhex(token, bytes, length, &decoded);
}

static int identity(token_t token, int optional, cfs_bf_id_t *out) {
    if (optional && TOKEN_EQUAL(token, "-")) {
        *out = (cfs_bf_id_t){0u, 0u};
        return CFS_BS_OK;
    }

    uint8_t bytes[CFS_BF_ID_BYTES];
    int rc = fixed_hex(token, bytes, sizeof(bytes));
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bf_id_t id = {cfs_get64be(bytes), cfs_get64be(bytes + 8u)};
    if (!cfs_bf_id_valid(id)) {
        return CFS_BS_ECORRUPT;
    }

    *out = id;
    return CFS_BS_OK;
}

static int metadata(cfs_bf_change_decoder_t *decoder, token_t token,
    uint32_t side, const cfs_bf_inode_t **out) {
    if (TOKEN_EQUAL(token, "-")) {
        *out = NULL;
        return CFS_BS_OK;
    }

    uint8_t *bytes = decoder->metadata[side];
    int rc = fixed_hex(token, bytes, CFS_BF_INODE_BYTES);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    int has_tree = cfs_get16(bytes) != CFS_BF_REGULAR &&
        cfs_get64(bytes + 24u) != 0u;
    rc = cfs_bf_inode_decode(bytes, CFS_BF_INODE_BYTES, has_tree,
        &decoder->inode[side]);
    if (rc != CFS_BS_OK || decoder->inode[side].links == 0u) {
        return CFS_BS_ECORRUPT;
    }

    *out = &decoder->inode[side];
    return CFS_BS_OK;
}

static int header(cfs_bf_change_decoder_t *decoder, const token_t *tokens,
    uint32_t count) {
    cfs_bf_change_header_t *h = &decoder->receipt.header;
    if (count != 6u || !TOKEN_EQUAL(tokens[0], "cfs-change-v1") ||
        fixed_hex(TOKEN_FIELD(tokens[1], "uuid="), h->uuid, sizeof(h->uuid)) !=
            CFS_BS_OK ||
        number(TOKEN_FIELD(tokens[2], "base="), &h->base) != CFS_BS_OK ||
        number(TOKEN_FIELD(tokens[3], "result="), &h->result) != CFS_BS_OK ||
        identity(TOKEN_FIELD(tokens[4], "root_before="), 0, &h->root_before) !=
            CFS_BS_OK ||
        identity(TOKEN_FIELD(tokens[5], "root_after="), 0, &h->root_after) !=
            CFS_BS_OK ||
        h->base == 0u || h->result == 0u || h->root_before.birth > h->base ||
        h->root_after.birth > h->result) {
        return CFS_BS_ECORRUPT;
    }

    decoder->state = CHANGES;
    return CFS_BS_OK;
}

static int record(cfs_bf_change_decoder_t *decoder, const token_t *tokens,
    uint32_t count) {
    cfs_bf_change_t *change = &decoder->change;
    memset(change, 0, sizeof(*change));
    if (count < 3u || identity(TOKEN_FIELD(tokens[1], "id="), 0, &change->id) !=
            CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    if (TOKEN_EQUAL(tokens[0], "inode") && count == 4u) {
        change->kind = CFS_BF_CHANGE_INODE;
        if (metadata(decoder, TOKEN_FIELD(tokens[2], "before="), 0u,
                &change->before) != CFS_BS_OK ||
            metadata(decoder, TOKEN_FIELD(tokens[3], "after="), 1u,
                &change->after) != CFS_BS_OK ||
            (change->before == NULL && change->after == NULL) ||
            (change->before != NULL && change->after != NULL &&
                (change->before->type != change->after->type ||
                    memcmp(decoder->metadata[0], decoder->metadata[1],
                        CFS_BF_INODE_BYTES) == 0))) {
            return CFS_BS_ECORRUPT;
        }
    } else if (TOKEN_EQUAL(tokens[0], "entry") && count == 5u) {
        change->kind = CFS_BF_CHANGE_ENTRY;
        change->bytes = decoder->bytes;
        if (unhex(TOKEN_FIELD(tokens[2], "name="), decoder->bytes, CFS_BF_NAME_MAX,
                &change->length) != CFS_BS_OK ||
            !cfs_bf_name_valid(change->bytes, (uint16_t)change->length) ||
            identity(TOKEN_FIELD(tokens[3], "before="), 1, &change->before_id) !=
                CFS_BS_OK ||
            identity(TOKEN_FIELD(tokens[4], "after="), 1, &change->after_id) !=
                CFS_BS_OK ||
            cfs_bf_id_equal(change->before_id, change->after_id)) {
            return CFS_BS_ECORRUPT;
        }
    } else if (TOKEN_EQUAL(tokens[0], "data") && count == 4u) {
        change->kind = CFS_BF_CHANGE_DATA;
        change->bytes = decoder->bytes;
        if (number(TOKEN_FIELD(tokens[2], "offset="), &change->offset) != CFS_BS_OK ||
            unhex(TOKEN_FIELD(tokens[3], "bytes="), decoder->bytes,
                CFS_BF_CHUNK_BYTES, &change->length) != CFS_BS_OK ||
            change->offset > INT64_MAX ||
            change->length > (uint64_t)INT64_MAX - change->offset) {
            return CFS_BS_ECORRUPT;
        }
    } else if (TOKEN_EQUAL(tokens[0], "symlink") && count == 3u) {
        change->kind = CFS_BF_CHANGE_SYMLINK;
        change->bytes = decoder->bytes;
        if (unhex(TOKEN_FIELD(tokens[2], "target="), decoder->bytes,
                CFS_BF_SYMLINK_MAX, &change->length) != CFS_BS_OK) {
            return CFS_BS_ECORRUPT;
        }

        for (uint32_t i = 0u; i < change->length; i++) {
            if (decoder->bytes[i] == 0u) {
                return CFS_BS_ECORRUPT;
            }
        }
    } else {
        return CFS_BS_ECORRUPT;
    }

    return cfs_bf_change_order(decoder);
}

static int end(cfs_bf_change_decoder_t *decoder, const token_t *tokens,
    uint32_t count) {
    uint64_t records;
    uint8_t hash[8];
    if (count != 3u || number(TOKEN_FIELD(tokens[1], "records="), &records) !=
            CFS_BS_OK || fixed_hex(TOKEN_FIELD(tokens[2], "hash="), hash,
            sizeof(hash)) != CFS_BS_OK ||
        records != decoder->receipt.records ||
        cfs_get64be(hash) != decoder->receipt.hash ||
        cfs_bf_change_group_end(&decoder->group) != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    decoder->state = ENDED;
    return CFS_BS_OK;
}

static int line(cfs_bf_change_decoder_t *decoder) {
    token_t tokens[6];
    uint32_t count = 0u;
    uint32_t start = 0u;
    uint32_t length = decoder->used - 1u;
    const uint8_t *bytes = decoder->line + 8u;
    for (uint32_t i = 0u; i <= length; i++) {
        if (i == length || bytes[i] == ' ') {
            if (start == i || count == 6u) {
                return CFS_BS_ECORRUPT;
            }

            tokens[count++] = (token_t){bytes + start, i - start};
            start = i + 1u;
        }
    }

    if (decoder->lines_decoded == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    decoder->lines_decoded++;
    if (decoder->state == CHANGES && TOKEN_EQUAL(tokens[0], "end")) {
        return end(decoder, tokens, count);
    }

    int is_header = decoder->state == NEED_HEADER;
    if ((!is_header && decoder->receipt.records == UINT64_MAX) ||
        decoder->hash_bytes > UINT64_MAX - 8u - decoder->used) {
        return CFS_BS_EOVERFLOW;
    }

    int rc = is_header ? header(decoder, tokens, count) :
        record(decoder, tokens, count);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_put64(decoder->line, decoder->receipt.hash);
    decoder->receipt.hash = cfs_hash64(decoder->line, 8u + decoder->used);
    decoder->hash_bytes += 8u + decoder->used;
    decoder->receipt.records += !is_header;
    rc = decoder->visit(decoder->context, &decoder->receipt.header,
        is_header ? NULL : &decoder->change);
    return rc == 0 ? decoder->status : CFS_BS_EINVAL;
}

int cfs_bf_change_start(cfs_bf_change_decoder_t *decoder,
    cfs_bf_change_visit_fn visit, void *context) {
    if (decoder == NULL || visit == NULL) {
        return CFS_BS_EINVAL;
    }

    memset(decoder, 0, sizeof(*decoder));
    decoder->state = NEED_HEADER;
    decoder->visit = visit;
    decoder->context = context;
    return CFS_BS_OK;
}

int cfs_bf_change_feed(cfs_bf_change_decoder_t *decoder,
    const void *input, uint32_t length) {
    if (!decoder) {
        return CFS_BS_EINVAL;
    }

    if (decoder->status != CFS_BS_OK) {
        return decoder->status;
    }

    if ((input == NULL && length != 0u) || decoder->active ||
        decoder->state < NEED_HEADER || decoder->state >= FINISHED) {
        decoder->status = CFS_BS_EINVAL;
        return decoder->status;
    }

    if (length > UINT64_MAX - decoder->bytes_consumed) {
        decoder->status = CFS_BS_EOVERFLOW;
        return decoder->status;
    }

    const uint8_t *bytes = input;
    decoder->active = 1u;
    for (uint32_t i = 0u; i < length && decoder->status == CFS_BS_OK; i++) {
        if (decoder->state == ENDED || decoder->used == CFS_BF_CHANGE_LINE_MAX) {
            decoder->status = CFS_BS_ECORRUPT;
            break;
        }

        decoder->line[8u + decoder->used++] = bytes[i];
        decoder->bytes_consumed++;
        if (bytes[i] == '\n') {
            decoder->status = line(decoder);
            decoder->used = 0u;
        }
    }

    decoder->active = 0u;
    return decoder->status;
}

int cfs_bf_change_finish(cfs_bf_change_decoder_t *decoder,
    cfs_bf_change_receipt_t *out) {
    if (!decoder) {
        return CFS_BS_EINVAL;
    }

    if (decoder->status != CFS_BS_OK) {
        return decoder->status;
    }

    if (out == NULL || decoder->active || decoder->state < NEED_HEADER ||
        decoder->state == FINISHED) {
        decoder->status = CFS_BS_EINVAL;
    } else if (decoder->state != ENDED || decoder->used != 0u) {
        decoder->status = CFS_BS_ECORRUPT;
    } else {
        *out = decoder->receipt;
        decoder->state = FINISHED;
    }

    return decoder->status;
}
