#define _POSIX_C_SOURCE 200809L

/* merkle: chunked SHA-256 Merkle checksums for save files.

   The save payload is split into fixed-size blocks. Every block becomes
   a leaf hash, leaves are merged pairwise layer by layer into a root,
   and the whole level-order tree lives in a trailing checksum region.
   Verification never writes to the payload: all entry points take the
   save buffer as const and only read it. A single block can be checked
   through its sibling path in O(log n), so a caller that knows which
   blocks changed pays only for those blocks.

   Domain separation keeps node classes unambiguous:
     leaf   = SHA256(0x00 || block bytes)
     pair   = SHA256(0x01 || left || right)
     pad    = SHA256(0x02)                      (synthetic padding leaf)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define MK_HASH_SZ    32
#define MK_VERSION    1u
#define MK_HDR_SZ     72                 /* fixed header inside the trailer */
#define MK_TAIL_SZ    48                 /* header digest + size + tail magic */
#define MK_MAX_DEPTH   32

#define MK_MAGIC      "MSMK"             /* head magic, 4 bytes             */
#define MK_TAIL_MAGIC "MSMKEND!"         /* 8 bytes                         */

typedef enum {
    MK_OK = 0,
    MK_ERR_SHORT,        /* buffer too small to hold a trailer       */
    MK_ERR_MAGIC,        /* head or tail magic corrupted             */
    MK_ERR_VERSION,      /* header version != MK_VERSION             */
    MK_ERR_BLOCK_SIZE,   /* block size is zero                       */
    MK_ERR_EMPTY,        /* payload has zero bytes                   */
    MK_ERR_LENGTH,       /* length / block count / trailer mismatch  */
    MK_ERR_TREE,         /* tree shape fields inconsistent           */
    MK_ERR_DIGEST,       /* header digest mismatch (forged metadata)  */
    MK_ERR_ROOT,         /* stored root or internal nodes corrupted  */
    MK_ERR_TAMPER,       /* one or more payload blocks fail proof    */
    MK_ERR_INDEX,        /* block index out of range                 */
    MK_ERR_NOMEM,
    MK_ERR_IO
} MkStatus;

static const char* mk_status_str(MkStatus st) {
    static const char* names[] = {
        "ok", "short", "magic", "version", "block_size", "empty",
        "length", "tree", "digest", "root", "tamper", "index", "nomem", "io"
    };
    return names[st];
}

/* ---------------- SHA-256 (streaming, libc only) ---------------- */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} Sha;

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha_init(Sha* s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->used = 0;
}

static void sha_transform(Sha* s, const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], hh = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void sha_update(Sha* s, const unsigned char* p, size_t n) {
    s->bits += (uint64_t)n * 8u;
    if (s->used) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha_transform(s, s->block);
            s->used = 0;
        }
    }
    while (n >= 64) {
        sha_transform(s, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(s->block, p, n);
        s->used = n;
    }
}

static void sha_final(Sha* s, unsigned char out[MK_HASH_SZ]) {
    uint64_t bits = s->bits;
    static const unsigned char pad_byte = 0x80;
    sha_update(s, &pad_byte, 1);
    unsigned char zeros[64] = {0};
    size_t gap = (64 - 8u - s->used) % 64;
    if (gap) sha_update(s, zeros, gap);
    unsigned char lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (unsigned char)(bits >> (56 - i * 8));
    sha_update(s, lenbe, 8);                /* used == 0 afterwards */
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

/* domain-separated node hashes */

static void hash_leaf(const unsigned char* data, size_t n,
                      unsigned char out[MK_HASH_SZ]) {
    Sha s;
    unsigned char tag = 0x00;
    sha_init(&s);
    sha_update(&s, &tag, 1);
    sha_update(&s, data, n);
    sha_final(&s, out);
}

static void hash_pair(const unsigned char left[MK_HASH_SZ],
                      const unsigned char right[MK_HASH_SZ],
                      unsigned char out[MK_HASH_SZ]) {
    Sha s;
    unsigned char tag = 0x01;
    sha_init(&s);
    sha_update(&s, &tag, 1);
    sha_update(&s, left, MK_HASH_SZ);
    sha_update(&s, right, MK_HASH_SZ);
    sha_final(&s, out);
}

static void hash_pad_leaf(unsigned char out[MK_HASH_SZ]) {
    Sha s;
    unsigned char tag = 0x02;
    sha_init(&s);
    sha_update(&s, &tag, 1);
    sha_final(&s, out);
}

static void hash_header(const unsigned char hdr[MK_HDR_SZ],
                        unsigned char out[MK_HASH_SZ]) {
    Sha s;
    unsigned char tag = 0x03;
    sha_init(&s);
    sha_update(&s, &tag, 1);
    sha_update(&s, hdr, MK_HDR_SZ);
    sha_final(&s, out);
}

/* ---------------- little-endian field helpers ---------------- */

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (i * 8));
}

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static size_t next_pow2(size_t x) {
    size_t p = 1;
    while (p < x) {
        if (p > ((size_t)-1) / 2) return 0;
        p <<= 1;
    }
    return p;
}

/* ---------------- parsed save context ---------------- */

typedef struct {
    const unsigned char* buf;        /* whole save, owned by caller  */
    size_t size;
    size_t trailer_off;             /* payload length in bytes      */
    const unsigned char* nodes;     /* level-order tree in trailer  */
    size_t level_off[MK_MAX_DEPTH + 1];
    size_t nblocks;
    size_t pow2;
    size_t depth;
    size_t node_count;
    uint32_t block_size;
    uint64_t payload_len;
    unsigned char root[MK_HASH_SZ];
    size_t leaf_hashes;             /* payload blocks actually hashed */
    int tree_tampered;              /* internal/root nodes disagree   */
} MkCtx;

/* one sibling on the leaf->root path */
typedef struct {
    unsigned char sibling[MK_HASH_SZ];
    int sibling_on_left;            /* 1: sibling is the left child  */
} MkStep;

/* ---------------- build: payload -> payload + trailer ---------------- */

static MkStatus mk_build(const unsigned char* payload, uint64_t len64,
                         uint32_t block_size,
                         unsigned char** out_buf, size_t* out_len) {
    if (block_size == 0) return MK_ERR_BLOCK_SIZE;
    if (len64 == 0) return MK_ERR_EMPTY;
    size_t len = (size_t)len64;

    size_t nblocks = len / block_size + (len % block_size != 0);
    size_t pow2 = next_pow2(nblocks);
    if (pow2 == 0) return MK_ERR_LENGTH;

    size_t depth = 0;
    while (((size_t)1 << depth) < pow2) depth++;
    if (depth > MK_MAX_DEPTH) return MK_ERR_LENGTH;

    size_t node_count = 2 * pow2 - 1;
    if (node_count > (SIZE_MAX - MK_HDR_SZ - MK_TAIL_SZ) / MK_HASH_SZ)
        return MK_ERR_LENGTH;
    size_t trailer = MK_HDR_SZ + node_count * MK_HASH_SZ + MK_TAIL_SZ;
    if (trailer > SIZE_MAX - len) return MK_ERR_LENGTH;
    size_t total = len + trailer;

    unsigned char* buf = malloc(total);
    if (!buf) return MK_ERR_NOMEM;
    memcpy(buf, payload, len);
    memset(buf + len, 0, trailer);          /* no uninitialized bytes */

    unsigned char* hdr = buf + len;
    unsigned char* nodes = hdr + MK_HDR_SZ;

    size_t off[MK_MAX_DEPTH + 1];
    off[0] = 0;
    for (size_t h = 1; h <= depth; h++)
        off[h] = off[h - 1] + (pow2 >> (h - 1));

    unsigned char pad[MK_HASH_SZ];
    hash_pad_leaf(pad);

    for (size_t i = 0; i < pow2; i++) {
        unsigned char* dst = nodes + (off[0] + i) * MK_HASH_SZ;
        if (i < nblocks) {
            size_t start = i * (size_t)block_size;
            size_t blen = len - start;
            if (blen > block_size) blen = block_size;
            hash_leaf(buf + start, blen, dst);
        } else {
            memcpy(dst, pad, MK_HASH_SZ);
        }
    }
    for (size_t h = 1; h <= depth; h++) {
        size_t cnt = pow2 >> h;
        for (size_t j = 0; j < cnt; j++) {
            const unsigned char* l = nodes + (off[h - 1] + 2 * j) * MK_HASH_SZ;
            const unsigned char* r = l + MK_HASH_SZ;
            hash_pair(l, r, nodes + (off[h] + j) * MK_HASH_SZ);
        }
    }

    memcpy(hdr, MK_MAGIC, 4);
    put32(hdr + 4, MK_VERSION);
    put32(hdr + 8, block_size);
    put32(hdr + 12, 0);                                  /* flags */
    put64(hdr + 16, len64);
    put32(hdr + 24, (uint32_t)nblocks);
    put32(hdr + 28, (uint32_t)pow2);
    put32(hdr + 32, (uint32_t)depth);
    put32(hdr + 36, (uint32_t)node_count);
    memcpy(hdr + 40, nodes + off[depth] * MK_HASH_SZ, MK_HASH_SZ);

    unsigned char* tail = hdr + MK_HDR_SZ + node_count * MK_HASH_SZ;
    hash_header(hdr, tail);                 /* authenticates every header byte */
    put64(tail + MK_HASH_SZ, trailer);
    memcpy(tail + MK_HASH_SZ + 8, MK_TAIL_MAGIC, 8);

    *out_buf = buf;
    *out_len = total;
    return MK_OK;
}

/* ---------------- open: parse and structurally validate ---------------- */

static MkStatus mk_open(const unsigned char* buf, size_t size, MkCtx* c) {
    memset(c, 0, sizeof(*c));
    c->buf = buf;
    c->size = size;

    if (size < MK_TAIL_SZ) return MK_ERR_SHORT;
    const unsigned char* tail = buf + size - MK_TAIL_SZ;
    if (memcmp(tail + MK_HASH_SZ + 8, MK_TAIL_MAGIC, 8) != 0)
        return MK_ERR_MAGIC;

    uint64_t trailer64 = get64(tail + MK_HASH_SZ);
    if (trailer64 < MK_HDR_SZ + MK_TAIL_SZ ||
        trailer64 > (uint64_t)size)
        return MK_ERR_LENGTH;
    size_t trailer = (size_t)trailer64;
    size_t head = size - trailer;
    const unsigned char* hdr = buf + head;
    if (memcmp(hdr, MK_MAGIC, 4) != 0) return MK_ERR_MAGIC;

    uint32_t version = get32(hdr + 4);
    if (version != MK_VERSION) return MK_ERR_VERSION;

    uint32_t block_size = get32(hdr + 8);
    if (block_size == 0) return MK_ERR_BLOCK_SIZE;
    /* flags (hdr+12) is reserved and authenticated solely by the digest */

    uint64_t payload_len = get64(hdr + 16);
    if (payload_len != (uint64_t)head) return MK_ERR_LENGTH;
    if (payload_len == 0) return MK_ERR_EMPTY;

    uint32_t nb = get32(hdr + 24);
    uint32_t pw = get32(hdr + 28);
    uint32_t dp = get32(hdr + 32);
    uint32_t nc = get32(hdr + 36);
    if (nb == 0 || pw == 0 || dp > MK_MAX_DEPTH || nc == 0)
        return MK_ERR_TREE;

    uint64_t expect_blocks = payload_len / block_size +
                             (payload_len % block_size != 0);
    if (expect_blocks != (uint64_t)nb) return MK_ERR_LENGTH;
    if (next_pow2(nb) != (size_t)pw) return MK_ERR_TREE;
    if ((size_t)1u << dp != (size_t)pw) return MK_ERR_TREE;
    if ((uint64_t)nc != 2u * (uint64_t)pw - 1u) return MK_ERR_TREE;
    if (trailer != MK_HDR_SZ + (size_t)nc * MK_HASH_SZ + MK_TAIL_SZ)
        return MK_ERR_LENGTH;

    /* structural checks reject malformed saves with specific codes; the
       digest is the cryptographic backstop that rejects a forged header
       which still happens to satisfy every cross-check above */
    unsigned char hdigest[MK_HASH_SZ];
    hash_header(hdr, hdigest);
    if (memcmp(hdigest, tail, MK_HASH_SZ) != 0) return MK_ERR_DIGEST;

    c->trailer_off = head;
    c->nodes = hdr + MK_HDR_SZ;
    c->nblocks = nb;
    c->pow2 = pw;
    c->depth = dp;
    c->node_count = nc;
    c->block_size = block_size;
    c->payload_len = payload_len;

    c->level_off[0] = 0;
    for (size_t h = 1; h <= c->depth; h++)
        c->level_off[h] = c->level_off[h - 1] + (c->pow2 >> (h - 1));

    memcpy(c->root, hdr + 40, MK_HASH_SZ);
    const unsigned char* root_node =
        c->nodes + (c->level_off[c->depth]) * MK_HASH_SZ;
    if (memcmp(c->root, root_node, MK_HASH_SZ) != 0)
        return MK_ERR_ROOT;

    return MK_OK;
}

/* ---------------- block-scoped verification (Merkle proof) ---------------- */

/* Recompute one block's leaf and walk its stored sibling path to the root.
   Fills steps[0..depth-1] even when the proof fails, so a tampered block
   arrives together with the exact sibling path that rejected it. */
static MkStatus mk_verify_block(MkCtx* c, size_t idx, MkStep* steps) {
    if (idx >= c->nblocks) return MK_ERR_INDEX;

    size_t start = idx * (size_t)c->block_size;
    size_t blen = (size_t)c->payload_len - start;
    if (blen > c->block_size) blen = c->block_size;

    unsigned char cur[MK_HASH_SZ];
    hash_leaf(c->buf + start, blen, cur);
    c->leaf_hashes++;

    for (size_t h = 0; h < c->depth; h++) {
        size_t pos = idx >> h;
        size_t sib_pos = pos ^ 1;
        const unsigned char* sib =
            c->nodes + (c->level_off[h] + sib_pos) * MK_HASH_SZ;
        if (steps) {
            memcpy(steps[h].sibling, sib, MK_HASH_SZ);
            steps[h].sibling_on_left = (sib_pos < pos);
        }
        unsigned char nxt[MK_HASH_SZ];
        if (sib_pos < pos) hash_pair(sib, cur, nxt);
        else               hash_pair(cur, sib, nxt);
        memcpy(cur, nxt, MK_HASH_SZ);
    }
    return memcmp(cur, c->root, MK_HASH_SZ) == 0 ? MK_OK : MK_ERR_TAMPER;
}

/* ---------------- full verification, O(n) ---------------- */

/* Re-hashes every payload block and rebuilds every internal layer from the
   stored leaves, so every payload byte and every trailer tree byte is
   covered. Returns the indices of every failing block; tree_tampered is
   set when the payload is intact but the checksum region itself is wrong. */
static MkStatus mk_verify_full(MkCtx* c,
                               size_t* bad, size_t bad_cap, size_t* bad_count) {
    *bad_count = 0;
    c->tree_tampered = 0;

    unsigned char expect[MK_HASH_SZ];
    for (size_t i = 0; i < c->nblocks; i++) {
        size_t start = i * (size_t)c->block_size;
        size_t blen = (size_t)c->payload_len - start;
        if (blen > c->block_size) blen = c->block_size;
        hash_leaf(c->buf + start, blen, expect);
        c->leaf_hashes++;
        const unsigned char* stored =
            c->nodes + (c->level_off[0] + i) * MK_HASH_SZ;
        if (memcmp(expect, stored, MK_HASH_SZ) != 0 && *bad_count < bad_cap)
            bad[(*bad_count)++] = i;
    }

    unsigned char pad[MK_HASH_SZ];
    hash_pad_leaf(pad);
    for (size_t i = c->nblocks; i < c->pow2; i++) {
        const unsigned char* stored =
            c->nodes + (c->level_off[0] + i) * MK_HASH_SZ;
        if (memcmp(pad, stored, MK_HASH_SZ) != 0) c->tree_tampered = 1;
    }

    for (size_t h = 1; h <= c->depth; h++) {
        size_t cnt = c->pow2 >> h;
        for (size_t j = 0; j < cnt; j++) {
            const unsigned char* l =
                c->nodes + (c->level_off[h - 1] + 2 * j) * MK_HASH_SZ;
            hash_pair(l, l + MK_HASH_SZ, expect);
            const unsigned char* stored =
                c->nodes + (c->level_off[h] + j) * MK_HASH_SZ;
            if (memcmp(expect, stored, MK_HASH_SZ) != 0)
                c->tree_tampered = 1;
        }
    }

    if (*bad_count) return MK_ERR_TAMPER;
    if (c->tree_tampered) return MK_ERR_ROOT;
    return MK_OK;
}

/* ---------------- changed-only verification ---------------- */

/* Verify exactly the caller-supplied dirty block indices via their sibling
   proofs; every other block is skipped and never hashed. An untouched file
   needs a full verify first; this is the cheap re-check for known patches. */
static MkStatus mk_verify_changed(MkCtx* c,
                                  const size_t* dirty, size_t dirty_count,
                                  size_t* bad, size_t bad_cap,
                                  size_t* bad_count, size_t* checked) {
    *bad_count = 0;
    if (checked) *checked = 0;
    for (size_t k = 0; k < dirty_count; k++) {
        if (dirty[k] >= c->nblocks) return MK_ERR_INDEX;
        if (checked) (*checked)++;
        MkStatus st = mk_verify_block(c, dirty[k], NULL);
        if (st == MK_ERR_TAMPER && *bad_count < bad_cap)
            bad[(*bad_count)++] = dirty[k];
        else if (st != MK_OK)
            return st;
    }
    return *bad_count ? MK_ERR_TAMPER : MK_OK;
}

/* Copy the stored (root-committed) leaf hashes for callers that want to
   diff trailer leaves between two versions of a save. */
static MkStatus mk_snapshot_leaves(const MkCtx* c, unsigned char* out) {
    if (!out) return MK_ERR_INDEX;
    memcpy(out, c->nodes, c->nblocks * MK_HASH_SZ);
    return MK_OK;
}

/* ---------------- thin file wrappers ---------------- */

static MkStatus mk_write_file(const char* path,
                              const unsigned char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return MK_ERR_IO;
    size_t wrote = fwrite(data, 1, len, f);
    int closed = fclose(f);
    return (wrote == len && closed == 0) ? MK_OK : MK_ERR_IO;
}

static MkStatus mk_read_file(const char* path,
                             unsigned char** out_buf, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return MK_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return MK_ERR_IO; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return MK_ERR_IO; }
    rewind(f);
    size_t len = (size_t)end;
    unsigned char* buf = malloc(len ? len : 1);
    if (!buf) { fclose(f); return MK_ERR_NOMEM; }
    size_t got = fread(buf, 1, len, f);
    int closed = fclose(f);
    if (got != len || closed != 0) { free(buf); return MK_ERR_IO; }
    *out_buf = buf;
    *out_len = len;
    return MK_OK;
}

/* ============================ self test ============================ */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int hex_eq(const unsigned char* h, const char* hex) {
    static const char* d = "0123456789abcdef";
    for (int i = 0; i < MK_HASH_SZ; i++) {
        if (d[h[i] >> 4] != hex[i * 2] || d[h[i] & 15] != hex[i * 2 + 1])
            return 0;
    }
    return 1;
}

static void print_hex8(const unsigned char* p) {
    for (int i = 0; i < 8; i++) printf("%02x", p[i]);
}

int main(void) {
    /* --- SHA-256 known-answer vectors --- */
    struct { const char* msg; const char* want; } vectors[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" }
    };
    int sha_ok = 1;
    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
        Sha s;
        unsigned char d[MK_HASH_SZ];
        sha_init(&s);
        sha_update(&s, (const unsigned char*)vectors[v].msg,
                   strlen(vectors[v].msg));
        sha_final(&s, d);
        if (!hex_eq(d, vectors[v].want)) sha_ok = 0;
    }
    check("sha256_vectors", sha_ok);

    /* every status code must have a human-readable name */
    int str_ok = 1;
    for (int i = 0; i <= MK_ERR_IO; i++)
        if (mk_status_str((MkStatus)i) == NULL) str_ok = 0;
    check("status_strings", str_ok);

    /* --- build argument validation --- */
    unsigned char* dummy = malloc(256);
    memset(dummy, 0xAB, 256);
    unsigned char* tmp = NULL;
    size_t tmp_len = 0;
    check("build_zero_block_size",
          mk_build(dummy, 256, 0, &tmp, &tmp_len) == MK_ERR_BLOCK_SIZE);
    check("build_empty_payload",
          mk_build(dummy, 0, 64, &tmp, &tmp_len) == MK_ERR_EMPTY);

    /* --- matrix of payload/block sizes: full verify must never false-alarm --- */
    size_t payload_sizes[] = { 1, 2, 7, 255, 256, 257, 1000, 4097 };
    uint32_t block_sizes[] = { 1, 7, 256, 1024 };
    int matrix_ok = 1;
    int depth0_ok = 1;
    int proof_ok = 1;
    int deterministic_ok = 1;
    for (size_t pi = 0; pi < sizeof(payload_sizes) / sizeof(payload_sizes[0]);
         pi++) {
        size_t plen = payload_sizes[pi];
        unsigned char* payload = malloc(plen);
        for (size_t i = 0; i < plen; i++) payload[i] = (unsigned char)(i * 31 + 7);
        for (size_t bi = 0; bi < sizeof(block_sizes) / sizeof(block_sizes[0]);
             bi++) {
            uint32_t bs = block_sizes[bi];
            unsigned char* a = NULL; size_t alen = 0;
            unsigned char* b2 = NULL; size_t blen2 = 0;
            if (mk_build(payload, plen, bs, &a, &alen) != MK_OK ||
                mk_build(payload, plen, bs, &b2, &blen2) != MK_OK) {
                matrix_ok = 0; free(a); free(b2); continue;
            }
            if (alen != blen2 || memcmp(a, b2, alen) != 0)
                deterministic_ok = 0;
            free(b2);

            MkCtx c;
            if (mk_open(a, alen, &c) != MK_OK) { matrix_ok = 0; free(a); continue; }
            size_t bad[64], nbad = 0;
            MkStatus st = mk_verify_full(&c, bad, 64, &nbad);
            if (st != MK_OK || nbad != 0) matrix_ok = 0;

            /* every block's sibling path must recompute the stored root */
            for (size_t idx = 0; idx < c.nblocks; idx++) {
                MkStep steps[MK_MAX_DEPTH];
                MkCtx c2;
                mk_open(a, alen, &c2);
                if (mk_verify_block(&c2, idx, steps) != MK_OK) proof_ok = 0;
                if (idx == 0 && c2.depth == 0) depth0_ok = 1;
                /* steps must point at the actual stored sibling nodes */
                for (size_t h = 0; h < c2.depth; h++) {
                    size_t pos = idx >> h;
                    size_t sib_pos = pos ^ 1;
                    const unsigned char* sp =
                        c2.nodes + (c2.level_off[h] + sib_pos) * MK_HASH_SZ;
                    if (memcmp(steps[h].sibling, sp, MK_HASH_SZ) != 0 ||
                        steps[h].sibling_on_left != (sib_pos < pos))
                        proof_ok = 0;
                }
            }
            free(a);
        }
        free(payload);
    }
    check("full_verify_no_false_positive", matrix_ok);
    check("build_deterministic", deterministic_ok);
    check("proof_paths_reach_root", proof_ok);
    check("single_block_depth_zero", depth0_ok);

    /* --- tamper localization: every block, several byte positions --- */
    size_t plen = 5000;
    uint32_t bs = 300;                       /* 17 blocks, last one short */
    unsigned char* payload = malloc(plen);
    for (size_t i = 0; i < plen; i++) payload[i] = (unsigned char)(i ^ 0x5A);
    unsigned char* save = NULL; size_t save_len = 0;
    check("build_main", mk_build(payload, plen, bs, &save, &save_len) == MK_OK);

    MkCtx base;
    check("open_main", mk_open(save, save_len, &base) == MK_OK);
    size_t expect_blocks = (plen + bs - 1) / bs;
    check("block_count", base.nblocks == expect_blocks && expect_blocks == 17);

    int locate_ok = 1;
    int neighbor_ok = 1;
    int changed_only_ok = 1;
    MkStep tamper_steps[MK_MAX_DEPTH];
    int got_tamper_proof = 0;
    for (size_t victim = 0; victim < base.nblocks; victim++) {
        size_t positions[] = {
            victim * bs,
            victim * bs + (bs / 2 < plen - victim * bs ? bs / 2 : 0),
            (victim + 1) * bs - 1 < plen ? (victim + 1) * bs - 1
                                         : plen - 1
        };
        for (size_t k = 0; k < 3; k++) {
            size_t pos = positions[k];
            if (pos < victim * bs || pos >= plen) continue;
            unsigned char* copy = malloc(save_len);
            memcpy(copy, save, save_len);
            copy[pos] ^= 0x01;               /* flip one payload byte */

            MkCtx c;
            if (mk_open(copy, save_len, &c) != MK_OK) { locate_ok = 0; free(copy); continue; }
            size_t bad[64], nbad = 0;
            MkStatus st = mk_verify_full(&c, bad, 64, &nbad);
            if (st != MK_ERR_TAMPER || nbad != 1 || bad[0] != victim)
                locate_ok = 0;

            MkStep steps[MK_MAX_DEPTH];
            if (mk_verify_block(&c, victim, steps) != MK_ERR_TAMPER)
                locate_ok = 0;
            if (victim == 3 && k == 0) {
                memcpy(tamper_steps, steps, c.depth * sizeof(MkStep));
                got_tamper_proof = (int)c.depth;
            }

            /* a neighboring block must still verify on the same save */
            size_t other = victim == 0 ? 1 : victim - 1;
            if (mk_verify_block(&c, other, NULL) != MK_OK) neighbor_ok = 0;

            /* changed-only: checking the victim catches it, and the
               number of hashed blocks equals the dirty list length */
            size_t dirty[2] = { victim, other };
            size_t dbad[64], dnbad = 0, checked = 0;
            c.leaf_hashes = 0;
            st = mk_verify_changed(&c, dirty, 2, dbad, 64, &dnbad, &checked);
            if (st != MK_ERR_TAMPER || dnbad != 1 || dbad[0] != victim ||
                checked != 2 || c.leaf_hashes != 2)
                changed_only_ok = 0;
            free(copy);
        }
    }
    check("tamper_localized_to_block", locate_ok);
    check("neighbor_still_valid", neighbor_ok);
    check("changed_only_dirty_set", changed_only_ok);

    /* print the sibling path of one tampered block */
    printf("tamper_block=3 depth=%d proof:\n", got_tamper_proof);
    for (int h = 0; h < got_tamper_proof; h++) {
        printf("  level=%d sibling_on_left=%d sibling=",
               h, tamper_steps[h].sibling_on_left);
        print_hex8(tamper_steps[h].sibling);
        printf("...\n");
    }

    /* --- changed-only hashes no block outside the dirty list --- */
    {
        MkCtx c;
        mk_open(save, save_len, &c);
        c.leaf_hashes = 0;
        size_t dirty[3] = { 0, 5, 16 };
        size_t bad[8], nbad = 0, checked = 0;
        MkStatus st = mk_verify_changed(&c, dirty, 3, bad, 8, &nbad, &checked);
        check("changed_only_clean", st == MK_OK && nbad == 0 && checked == 3);
        check("changed_only_hash_count", c.leaf_hashes == 3);

        c.leaf_hashes = 0;
        st = mk_verify_changed(&c, NULL, 0, bad, 8, &nbad, &checked);
        check("changed_only_empty_set", st == MK_OK && checked == 0 &&
                                        c.leaf_hashes == 0);
        check("changed_only_bad_index",
              mk_verify_changed(&c, (size_t[]){ 999 }, 1, bad, 8, &nbad,
                                &checked) == MK_ERR_INDEX);
    }

    /* --- legitimate rewrite: proof must accept the new trailer --- */
    {
        unsigned char* np = malloc(plen);
        memcpy(np, payload, plen);
        np[3 * bs + 1] ^= 0x10;             /* known in-place patch */
        unsigned char* ns = NULL; size_t nsl = 0;
        check("rebuild_after_patch",
              mk_build(np, plen, bs, &ns, &nsl) == MK_OK);
        MkCtx c;
        check("open_rebuilt", mk_open(ns, nsl, &c) == MK_OK);
        size_t dirty[1] = { 3 };
        size_t bad[8], nbad = 0, checked = 0;
        check("changed_block_accepted",
              mk_verify_changed(&c, dirty, 1, bad, 8, &nbad, &checked) ==
                  MK_OK &&
              nbad == 0 && checked == 1);
        size_t leaves[17 * MK_HASH_SZ];
        check("snapshot_size", mk_snapshot_leaves(&c,
              (unsigned char*)leaves) == MK_OK);
        free(ns);
        free(np);
    }

    /* --- trailer attacks must produce specific errors, never a clean bill --- */
    {
        unsigned char* cbuf = malloc(save_len);
        MkCtx c;
        size_t bad[64], nbad = 0;

        memcpy(cbuf, save, save_len);
        put32(cbuf + base.trailer_off + 4, MK_VERSION + 1);
        check("version_mismatch", mk_open(cbuf, save_len, &c) == MK_ERR_VERSION);

        memcpy(cbuf, save, save_len);
        put32(cbuf + base.trailer_off + 8, 0);
        check("zero_block_size_header",
              mk_open(cbuf, save_len, &c) == MK_ERR_BLOCK_SIZE);

        memcpy(cbuf, save, save_len);
        put64(cbuf + base.trailer_off + 16, base.payload_len - 1);
        check("length_mismatch", mk_open(cbuf, save_len, &c) == MK_ERR_LENGTH);

        memcpy(cbuf, save, save_len);
        cbuf[save_len - 1] ^= 0x01;        /* tail magic */
        check("tail_magic_corrupt", mk_open(cbuf, save_len, &c) == MK_ERR_MAGIC);

        memcpy(cbuf, save, save_len);
        check("truncated_short",
              mk_open(cbuf, MK_TAIL_SZ - 1, &c) == MK_ERR_SHORT);
        check("truncated_one_byte", mk_open(cbuf, save_len - 1, &c) != MK_OK);
        check("garbage", mk_open((const unsigned char*)"x", 1, &c) ==
                             MK_ERR_SHORT);

        /* internal-node tamper: payload intact, checksum region wrong */
        memcpy(cbuf, save, save_len);
        size_t internal = base.trailer_off + MK_HDR_SZ +
                          (base.level_off[1] + 1) * MK_HASH_SZ;
        cbuf[internal] ^= 0x01;
        check("internal_tamper_open", mk_open(cbuf, save_len, &c) == MK_OK);
        check("internal_tamper_flagged",
              mk_verify_full(&c, bad, 64, &nbad) == MK_ERR_ROOT &&
              nbad == 0 && c.tree_tampered == 1);

        /* stored-leaf tamper without payload change: localizes that block */
        memcpy(cbuf, save, save_len);
        size_t leaf4 = base.trailer_off + MK_HDR_SZ +
                       (base.level_off[0] + 4) * MK_HASH_SZ;
        cbuf[leaf4] ^= 0x01;
        check("leaf_tamper_open", mk_open(cbuf, save_len, &c) == MK_OK);
        check("leaf_tamper_localized",
              mk_verify_full(&c, bad, 64, &nbad) == MK_ERR_TAMPER &&
              nbad == 1 && bad[0] == 4);

        /* header root copy tamper: the header digest catches it first */
        memcpy(cbuf, save, save_len);
        cbuf[base.trailer_off + 40] ^= 0x01;
        check("root_header_tamper_rejected",
              mk_open(cbuf, save_len, &c) == MK_ERR_DIGEST);

        /* tree root node tamper: header is intact, digest passes, the
           root copy in the header disagrees with the tree root */
        memcpy(cbuf, save, save_len);
        size_t root_node_off = base.trailer_off + MK_HDR_SZ +
                               base.level_off[base.depth] * MK_HASH_SZ;
        cbuf[root_node_off] ^= 0x01;
        check("root_node_tamper_rejected",
              mk_open(cbuf, save_len, &c) == MK_ERR_ROOT);

        /* the reserved flags field has no structural cross-check: the
           header digest is its only protection, so a forged flag must
           surface as MK_ERR_DIGEST rather than be silently accepted */
        memcpy(cbuf, save, save_len);
        put32(cbuf + base.trailer_off + 12, 0x40000000u);
        check("header_forgery_digest",
              mk_open(cbuf, save_len, &c) == MK_ERR_DIGEST);

        /* digest field itself corrupted */
        memcpy(cbuf, save, save_len);
        size_t digest_off = base.trailer_off + MK_HDR_SZ +
                            base.node_count * MK_HASH_SZ;
        cbuf[digest_off] ^= 0x01;
        check("digest_field_corrupt",
              mk_open(cbuf, save_len, &c) == MK_ERR_DIGEST);

        free(cbuf);
    }

    /* --- verification must not mutate any byte of the input buffer --- */
    {
        unsigned char* before = malloc(save_len);
        memcpy(before, save, save_len);
        MkCtx c;
        mk_open(save, save_len, &c);
        size_t bad[64], nbad = 0;
        mk_verify_full(&c, bad, 64, &nbad);
        for (size_t i = 0; i < c.nblocks; i++)
            mk_verify_block(&c, i, NULL);
        size_t dirty[2] = { 2, 9 };
        mk_verify_changed(&c, dirty, 2, bad, 64, &nbad, &(size_t){0});
        check("verify_is_read_only", memcmp(before, save, save_len) == 0);
        free(before);
    }

    /* --- on-disk round trip, tamper a byte on disk, reload, locate --- */
    {
        char path[] = "/tmp/merkle_selftest_XXXXXX";
        int fd = mkstemp(path);
        int file_ok = (fd >= 0);
        if (fd >= 0) close(fd);
        if (file_ok) file_ok =
            (mk_write_file(path, save, save_len) == MK_OK);
        unsigned char* disk = NULL; size_t disk_len = 0;
        if (file_ok) file_ok =
            (mk_read_file(path, &disk, &disk_len) == MK_OK &&
             disk_len == save_len && memcmp(disk, save, save_len) == 0);
        MkCtx c;
        if (file_ok) file_ok = (mk_open(disk, disk_len, &c) == MK_OK);
        size_t bad[64], nbad = 0;
        if (file_ok) file_ok =
            (mk_verify_full(&c, bad, 64, &nbad) == MK_OK);

        /* corrupt one byte in the middle of the file, re-read from disk */
        if (file_ok) {
            FILE* f = fopen(path, "r+b");
            file_ok = (f != NULL);
            if (f) {
                fseek(f, (long)(7 * bs + 11), SEEK_SET);
                fputc(fgetc(f) ^ 0x01, f);
                fclose(f);
            }
        }
        free(disk); disk = NULL;
        if (file_ok) file_ok =
            (mk_read_file(path, &disk, &disk_len) == MK_OK);
        if (file_ok) file_ok = (mk_open(disk, disk_len, &c) == MK_OK);
        if (file_ok) {
            MkStatus st = mk_verify_full(&c, bad, 64, &nbad);
            file_ok = (st == MK_ERR_TAMPER && nbad == 1 && bad[0] == 7);
        }
        check("file_roundtrip_and_tamper", file_ok);
        remove(path);
        free(disk);
    }

    free(save);
    free(payload);
    free(dummy);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
