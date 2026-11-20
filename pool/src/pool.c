
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused. Handles are fixed-size
   and survive save/load: the on-disk image keeps per-slot generations,
   and every saved reference is validated against them on load, so a
   generation mismatch rejects the whole load instead of resolving to a
   recycled object. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define POOL_CAP 16
#define POOL_MAX_REFS 64

/* On-disk image is little-endian regardless of host endianness. */
#define POOL_MAGIC   0x4C4F4F50u /* bytes 'P','O','O','L' */
#define POOL_VERSION 1u

typedef struct {
    uint32_t index;
    uint32_t gen;
} Handle;

/* A handle is part of the save format: its size must never drift. */
_Static_assert(sizeof(Handle) == 8, "Handle must stay a fixed 8 bytes");
_Static_assert(sizeof(int) == 4, "on-disk int32 values assume 4-byte int");

typedef struct {
    int in_use[POOL_CAP];
    uint32_t gen[POOL_CAP];
    int32_t value[POOL_CAP];
    int used;
    int high_water;
} Pool;

static void pool_init(Pool* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < POOL_CAP; i++) p->gen[i] = 1;
}

static Handle pool_alloc(Pool* p, int32_t v) {
    for (int i = 0; i < POOL_CAP; i++) {
        if (!p->in_use[i]) {
            p->in_use[i] = 1;
            p->value[i] = v;
            p->used++;
            if (p->used > p->high_water) p->high_water = p->used;
            Handle h = { (uint32_t)i, p->gen[i] };
            return h;
        }
    }
    Handle bad = { UINT32_MAX, 0 };
    return bad;
}

static int pool_valid(const Pool* p, Handle h) {
    if (h.index >= POOL_CAP) return 0;
    if (!p->in_use[h.index]) return 0;
    return p->gen[h.index] == h.gen;
}

static int pool_get(const Pool* p, Handle h, int32_t* out) {
    if (!pool_valid(p, h)) return -1;
    *out = p->value[h.index];
    return 0;
}

static int pool_free(Pool* p, Handle h) {
    if (!pool_valid(p, h)) return -1;
    p->in_use[h.index] = 0;
    p->gen[h.index]++;          /* bump so stale handles fail */
    p->used--;
    return 0;
}

/* ---- fixed-width little-endian (de)serialization ---- */

static void put_u32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put_i32(FILE* f, int32_t v) { put_u32(f, (uint32_t)v); }

static int get_u32(FILE* f, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int get_i32(FILE* f, int32_t* out) {
    uint32_t u;
    if (get_u32(f, &u)) return -1;
    *out = (int32_t)u;
    return 0;
}

/*
  File layout, every field fixed width:
    header:  u32 magic, u32 version, u32 cap, u32 used, u32 high_water
    slots:   cap * (u32 in_use, u32 gen, i32 value)
    refs:    u32 nref, then nref * (u32 index, u32 gen)  -- 8 bytes each
*/
static int pool_save(const Pool* p, const Handle* refs, uint32_t nref,
                     const char* path) {
    if (nref > POOL_MAX_REFS || (nref != 0 && refs == NULL)) return -1;
    for (uint32_t i = 0; i < nref; i++)
        if (!pool_valid(p, refs[i])) return -1;   /* no saving stale refs */

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof(tmp)) return -1;

    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;

    put_u32(f, POOL_MAGIC);
    put_u32(f, POOL_VERSION);
    put_u32(f, POOL_CAP);
    put_u32(f, (uint32_t)p->used);
    put_u32(f, (uint32_t)p->high_water);
    for (int i = 0; i < POOL_CAP; i++) {
        put_u32(f, (uint32_t)p->in_use[i]);
        put_u32(f, p->gen[i]);
        put_i32(f, p->value[i]);
    }
    put_u32(f, nref);
    for (uint32_t i = 0; i < nref; i++) {
        put_u32(f, refs[i].index);
        put_u32(f, refs[i].gen);
    }

    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/*
  Loads into `p` only after the whole image validates, so a rejected file
  leaves the target pool untouched. `refs` takes at most `cap_refs`
  saved handles; *out_nref reports how many were stored.
*/
static int pool_load(Pool* p, Handle* refs, uint32_t cap_refs,
                     uint32_t* out_nref, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    Pool img;
    Handle got[POOL_MAX_REFS];
    memset(&img, 0, sizeof(img));

    uint32_t magic, version, cap, used, hwm;
    if (get_u32(f, &magic) || get_u32(f, &version) || get_u32(f, &cap) ||
        get_u32(f, &used) || get_u32(f, &hwm)) { fclose(f); return -1; }
    if (magic != POOL_MAGIC || version != POOL_VERSION || cap != POOL_CAP) {
        fclose(f);
        return -1;
    }

    uint32_t live = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t in_use, gen;
        int32_t val;
        if (get_u32(f, &in_use) || get_u32(f, &gen) || get_i32(f, &val)) {
            fclose(f);
            return -1;
        }
        if (in_use > 1u || (in_use != 0 && gen == 0)) {  /* gen 0 is never live */
            fclose(f);
            return -1;
        }
        img.in_use[i] = (int)in_use;
        img.gen[i] = gen;
        img.value[i] = val;
        if (in_use) live++;
    }
    if (used != live || hwm < used || hwm > cap) { fclose(f); return -1; }
    img.used = (int)used;
    img.high_water = (int)hwm;

    uint32_t nref;
    if (get_u32(f, &nref)) { fclose(f); return -1; }
    if (nref > POOL_MAX_REFS || nref > cap_refs) { fclose(f); return -1; }
    for (uint32_t i = 0; i < nref; i++) {
        if (get_u32(f, &got[i].index) || get_u32(f, &got[i].gen)) {
            fclose(f);
            return -1;
        }
        /* generation conflict: a saved ref must name a live slot at the
           exact generation stored for it, never a recycled slot. */
        if (!pool_valid(&img, got[i])) { fclose(f); return -1; }
    }
    if (fgetc(f) != EOF) { fclose(f); return -1; }   /* trailing junk */
    fclose(f);

    *p = img;
    for (uint32_t i = 0; i < nref; i++) refs[i] = got[i];
    if (out_nref) *out_nref = nref;
    return 0;
}

static int copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return -1;
    FILE* out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[256];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) ok = 0;
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Pool p;
    pool_init(&p);

    Handle a = pool_alloc(&p, 10);
    Handle b = pool_alloc(&p, 20);
    int32_t v = 0;
    check("get_a", pool_get(&p, a, &v) == 0 && v == 10);
    check("get_b", pool_get(&p, b, &v) == 0 && v == 20);
    check("count", p.used == 2);

    check("free_a", pool_free(&p, a) == 0 && p.used == 1);
    check("stale_a", pool_get(&p, a, &v) == -1);

    Handle c = pool_alloc(&p, 30);
    check("reuse_slot", c.index == a.index);
    check("new_gen", c.gen != a.gen);
    check("stale_still_bad", pool_get(&p, a, &v) == -1);
    check("c_ok", pool_get(&p, c, &v) == 0 && v == 30);

    check("double_free", pool_free(&p, c) == 0 && pool_free(&p, c) == -1);

    Pool q;
    pool_init(&q);
    Handle hs[POOL_CAP];
    for (int i = 0; i < POOL_CAP; i++) hs[i] = pool_alloc(&q, i);
    Handle full = pool_alloc(&q, 99);
    check("exhausted", full.index == UINT32_MAX);
    check("high_water", q.high_water == POOL_CAP);
    for (int i = 0; i < POOL_CAP; i++) pool_free(&q, hs[i]);
    check("all_free", q.used == 0);
    check("handles_stale", pool_get(&q, hs[0], &v) == -1);

    /* ---- persistence: save, reload, per-object equivalence ---- */
    const char* good = "/tmp/mini_sim_pool_good.bin";
    const char* bad = "/tmp/mini_sim_pool_bad.bin";

    Pool s;
    pool_init(&s);
    Handle refs[POOL_MAX_REFS];
    int32_t want[POOL_MAX_REFS];
    uint32_t nref = 0;
    refs[nref] = pool_alloc(&s, 111); want[nref] = 111; nref++;
    refs[nref] = pool_alloc(&s, 222); want[nref] = 222; nref++;
    Handle doomed = pool_alloc(&s, 333);
    pool_free(&s, doomed);
    refs[nref] = pool_alloc(&s, 444); want[nref] = 444; nref++; /* recycled slot, new gen */
    refs[nref] = pool_alloc(&s, 555); want[nref] = 555; nref++;

    check("handle_fixed_size", sizeof(Handle) == 8);
    check("save_rejects_stale", pool_save(&s, &doomed, 1, bad) == -1);
    check("save", pool_save(&s, refs, nref, good) == 0);

    Pool t;
    pool_init(&t);
    Handle back[POOL_MAX_REFS];
    uint32_t got_n = 0;
    check("load", pool_load(&t, back, POOL_MAX_REFS, &got_n, good) == 0);
    check("ref_count", got_n == nref);

    int slots_same = (t.used == s.used && t.high_water == s.high_water);
    for (int i = 0; i < POOL_CAP && slots_same; i++)
        slots_same = (t.in_use[i] == s.in_use[i] &&
                      t.gen[i] == s.gen[i] &&
                      t.value[i] == s.value[i]);
    check("slots_identical", slots_same);

    int queries = 1;
    for (uint32_t i = 0; i < nref; i++) {
        int32_t rv;
        if (back[i].index != refs[i].index || back[i].gen != refs[i].gen) queries = 0;
        if (pool_get(&t, back[i], &rv) != 0 || rv != want[i]) queries = 0;
    }
    check("queries_match", queries);
    check("freed_stays_stale", pool_get(&t, doomed, &v) == -1);

    /* generations keep working after a reload */
    Handle first = back[0];
    check("post_load_free", pool_free(&t, first) == 0);
    check("post_load_stale", pool_get(&t, first, &v) == -1);
    Handle again = pool_alloc(&t, 777);
    check("post_load_reuse", again.index == first.index &&
                             again.gen == first.gen + 1);

    Handle tiny[1];
    check("ref_buf_too_small",
          pool_load(&t, tiny, 1, &got_n, good) == -1);

    /* generation conflict: corrupt one saved reference's generation */
    check("copy_for_tamper", copy_file(good, bad) == 0);
    FILE* bf = fopen(bad, "r+b");
    long ref0_gen_off = 5L * 4 + (long)POOL_CAP * 12 + 4 + 4;
    check("tamper_seek", bf != NULL && fseek(bf, ref0_gen_off, SEEK_SET) == 0);
    uint32_t bogus = 0xDEADBEEFu;
    fwrite(&bogus, sizeof(bogus), 1, bf);
    fclose(bf);

    Pool guard;
    pool_init(&guard);
    Handle gref = pool_alloc(&guard, 1);
    Handle gback[POOL_MAX_REFS];
    check("gen_conflict_rejected",
          pool_load(&guard, gback, POOL_MAX_REFS, &got_n, bad) == -1);
    int32_t gv = 0;
    check("rejected_unchanged", pool_get(&guard, gref, &gv) == 0 && gv == 1);

    /* corrupted magic must be rejected */
    check("copy_for_magic", copy_file(good, bad) == 0);
    bf = fopen(bad, "r+b");
    fseek(bf, 0, SEEK_SET);
    bogus = 0;
    fwrite(&bogus, sizeof(bogus), 1, bf);
    fclose(bf);
    check("bad_magic_rejected",
          pool_load(&guard, gback, POOL_MAX_REFS, &got_n, bad) == -1);

    /* truncated image: valid header, body cut off mid-slot */
    bf = fopen(bad, "wb");
    put_u32(bf, POOL_MAGIC);
    put_u32(bf, POOL_VERSION);
    put_u32(bf, POOL_CAP);
    put_u32(bf, 1);
    put_u32(bf, 1);
    fclose(bf);
    check("truncated_rejected",
          pool_load(&guard, gback, POOL_MAX_REFS, &got_n, bad) == -1);

    remove(good);
    remove(bad);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
