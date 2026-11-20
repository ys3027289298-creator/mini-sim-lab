
/* vpool: growable vertex buffer pool with generation handles.

   The pool stores triangles in a packed physical arena. Every live triangle
   owns a logical slot; a Handle names (logical slot, generation). When the
   arena fills up it is grown by compacting the live triangles into a new,
   larger arena. Three invariants make old handles survive a grow:

     1. slot data moves wholesale (Tri payload copied verbatim, nothing is
        reinitialized);
     2. generations are never reset on grow -- they are copied as-is, so
        stale handles stay stale and live handles stay live;
     3. the index map map[logical] -> physical is rebuilt in the same pass,
        so every live handle still resolves to its own vertex data.

   Stale or malformed handles fail validation and are never drawn: freed
   physical slots are scrubbed, so a failed submit cannot reach dirty data.

   The framebuffer rasterizer is integer-only and deterministic, which lets
   test runs be compared pixel-for-pixel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ types */

typedef struct {
    int x, y;
} Vertex;

typedef struct {
    Vertex v[3];
    unsigned int color;   /* 0x00RRGGBB */
} Tri;

typedef struct {
    unsigned int slot;    /* logical slot */
    unsigned int gen;     /* generation, 0 marks the sentinel bad handle */
} Handle;

#define BAD_HANDLE ((Handle){ (unsigned)-1, 0u })

typedef struct {
    int cap;
    int used;
    int high_water;
    int grows;

    unsigned int* gen;    /* gen[logical], always >= 1 for a valid handle */
    unsigned char* in_use;
    int* map;             /* index map: logical slot -> physical slot */
    Tri* arena;           /* packed physical vertex storage */
} VPool;

#define SCRUB 0xCDu

/* ------------------------------------------------------------- pool logic */

static int vpool_init(VPool* p, int cap) {
    if (cap < 1) return -1;
    memset(p, 0, sizeof(*p));
    p->cap = cap;
    p->gen = malloc((size_t)cap * sizeof(*p->gen));
    p->in_use = calloc((size_t)cap, sizeof(*p->in_use));
    p->map = malloc((size_t)cap * sizeof(*p->map));
    p->arena = malloc((size_t)cap * sizeof(*p->arena));
    if (!p->gen || !p->in_use || !p->map || !p->arena) {
        free(p->gen); free(p->in_use); free(p->map); free(p->arena);
        memset(p, 0, sizeof(*p));
        return -1;
    }
    for (int i = 0; i < cap; i++) {
        p->gen[i] = 1;          /* first allocation of every slot is gen 1 */
        p->map[i] = -1;
    }
    return 0;
}

static void vpool_destroy(VPool* p) {
    free(p->gen); free(p->in_use); free(p->map); free(p->arena);
    memset(p, 0, sizeof(*p));
}

/* Resolve a handle to its physical slot, or -1 if it is stale/malformed. */
static int vpool_resolve(const VPool* p, Handle h) {
    if (h.gen == 0) return -1;
    if (h.slot >= (unsigned)p->cap) return -1;
    if (!p->in_use[h.slot]) return -1;
    if (p->gen[h.slot] != h.gen) return -1;
    int phys = p->map[h.slot];
    if (phys < 0 || phys >= p->used) return -1;   /* map must be in sync */
    return phys;
}

static int vpool_valid(const VPool* p, Handle h) {
    return vpool_resolve(p, h) >= 0;
}

/* Grow: compact live slots into a doubled arena.
   Wholesale move, generations preserved, index map rebuilt. */
static int vpool_grow(VPool* p) {
    int old_cap = p->cap;
    int new_cap = old_cap * 2;

    unsigned int* gen = malloc((size_t)new_cap * sizeof(*gen));
    unsigned char* in_use = calloc((size_t)new_cap, sizeof(*in_use));
    int* map = malloc((size_t)new_cap * sizeof(*map));
    Tri* arena = malloc((size_t)new_cap * sizeof(*arena));
    if (!gen || !in_use || !map || !arena) {
        free(gen); free(in_use); free(map); free(arena);
        return -1;
    }

    /* invariant 1+2+3: move every live slot wholesale, keep its generation,
       and point the rebuilt index map at the new physical location. */
    int dest = 0;
    for (int i = 0; i < old_cap; i++) {
        gen[i] = p->gen[i];                 /* never reset generations */
        if (p->in_use[i]) {
            arena[dest] = p->arena[p->map[i]];  /* verbatim vertex data */
            in_use[i] = 1;
            map[i] = dest;
            dest++;
        } else {
            map[i] = -1;
        }
    }
    /* brand-new slots start at generation 1, unmapped */
    for (int i = old_cap; i < new_cap; i++) {
        gen[i] = 1;
        map[i] = -1;
    }
    /* the compaction is dense: dest must equal the live count */
    if (dest != p->used) {
        free(gen); free(in_use); free(map); free(arena);
        return -1;
    }

    free(p->gen); free(p->in_use); free(p->map); free(p->arena);
    p->gen = gen;
    p->in_use = in_use;
    p->map = map;
    p->arena = arena;
    p->cap = new_cap;
    p->grows++;
    return 0;
}

static Handle vpool_alloc(VPool* p, Tri t) {
    if (p->used >= p->cap) {
        if (vpool_grow(p) != 0) return BAD_HANDLE;
    }
    int slot = -1;
    for (int i = 0; i < p->cap; i++) {
        if (!p->in_use[i]) { slot = i; break; }
    }
    if (slot < 0) return BAD_HANDLE;

    int phys = p->used;                     /* arena stays packed */
    p->arena[phys] = t;
    p->map[slot] = phys;
    p->in_use[slot] = 1;
    p->used++;
    if (p->used > p->high_water) p->high_water = p->used;
    Handle h = { (unsigned)slot, p->gen[slot] };
    return h;
}

static int vpool_free(VPool* p, Handle h) {
    int phys = vpool_resolve(p, h);
    if (phys < 0) return -1;

    /* keep the arena packed: move the last live triangle into the hole */
    int last = p->used - 1;
    if (phys != last) {
        p->arena[phys] = p->arena[last];
        for (int i = 0; i < p->cap; i++) {
            if (p->in_use[i] && p->map[i] == last) {
                p->map[i] = phys;          /* index map follows the data */
                break;
            }
        }
    }
    memset(&p->arena[last], SCRUB, sizeof(Tri));   /* no dirty data left */
    p->in_use[h.slot] = 0;
    p->map[h.slot] = -1;
    p->gen[h.slot]++;                      /* stale handles must fail */
    p->used--;
    return 0;
}

static int vpool_update(VPool* p, Handle h, Tri t) {
    int phys = vpool_resolve(p, h);
    if (phys < 0) return -1;
    p->arena[phys] = t;
    return 0;
}

static int vpool_get(const VPool* p, Handle h, Tri* out) {
    int phys = vpool_resolve(p, h);
    if (phys < 0) return -1;
    *out = p->arena[phys];
    return 0;
}

/* -------------------------------------------------------- tiny rasterizer */

#define FW 64
#define FH 48
#define BG 0x00101018u

typedef struct {
    unsigned int px[FW * FH];
} Frame;

static void frame_clear(Frame* f) {
    for (int i = 0; i < FW * FH; i++) f->px[i] = BG;
}

/* Integer edge function; fill rule is sign-agnostic so winding does not
   matter. Coordinates are doubled so pixel centers land on integers. */
static long edge_fn(Vertex a, Vertex b, int px, int py) {
    return (long)(b.x - a.x) * (py * 2 - a.y * 2)
         - (long)(b.y - a.y) * (px * 2 - a.x * 2);
}

static void frame_draw(Frame* f, const Tri* t) {
    int minx = t->v[0].x, maxx = t->v[0].x;
    int miny = t->v[0].y, maxy = t->v[0].y;
    for (int k = 1; k < 3; k++) {
        if (t->v[k].x < minx) minx = t->v[k].x;
        if (t->v[k].x > maxx) maxx = t->v[k].x;
        if (t->v[k].y < miny) miny = t->v[k].y;
        if (t->v[k].y > maxy) maxy = t->v[k].y;
    }
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= FW) maxx = FW - 1;
    if (maxy >= FH) maxy = FH - 1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            long e0 = edge_fn(t->v[0], t->v[1], x, y);
            long e1 = edge_fn(t->v[1], t->v[2], x, y);
            long e2 = edge_fn(t->v[2], t->v[0], x, y);
            int inside = (e0 >= 0 && e1 >= 0 && e2 >= 0)
                      || (e0 <= 0 && e1 <= 0 && e2 <= 0);
            if (inside) f->px[y * FW + x] = t->color;
        }
    }
}

/* Submit a handle: returns -1 (and touches no pixels) when stale. */
static int vpool_submit(const VPool* p, Handle h, Frame* f) {
    int phys = vpool_resolve(p, h);
    if (phys < 0) return -1;
    frame_draw(f, &p->arena[phys]);
    return 0;
}

/* ------------------------------------------------------------ self tests */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static Tri mk_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                  unsigned int color) {
    Tri t;
    t.v[0].x = x0; t.v[0].y = y0;
    t.v[1].x = x1; t.v[1].y = y1;
    t.v[2].x = x2; t.v[2].y = y2;
    t.color = color;
    return t;
}

/* One deterministic scene, replayed on two pools: a small one that must
   grow repeatedly, and a large one that never grows. Both must render the
   exact same framebuffer. Frees deliberately target non-last live slots so
   physical holes exist and a grow has to compact (move data, rebuild map). */
#define N_TRI 11

static void build_scene(VPool* p, Handle* hs, Tri* tris) {
    hs[0] = vpool_alloc(p, tris[0] = mk_tri(2, 2, 14, 4, 6, 16, 0x00FF3030));
    hs[1] = vpool_alloc(p, tris[1] = mk_tri(20, 6, 34, 8, 26, 22, 0x0030FF30));
    hs[2] = vpool_alloc(p, tris[2] = mk_tri(40, 4, 58, 10, 46, 24, 0x003060FF));
    hs[3] = vpool_alloc(p, tris[3] = mk_tri(8, 26, 24, 30, 12, 44, 0x00FFFF30));
    /* free a non-last live slot: physical hole with live data after it */
    (void)vpool_free(p, hs[3]);
    hs[4] = vpool_alloc(p, tris[4] = mk_tri(28, 26, 44, 30, 34, 44, 0x00FF30FF));
    hs[5] = vpool_alloc(p, tris[5] = mk_tri(48, 28, 62, 32, 52, 46, 0x0030FFFF));

    hs[6] = vpool_alloc(p, tris[6] = mk_tri(14, 10, 22, 12, 18, 20, 0x00C08040));
    Handle mid = vpool_alloc(p, mk_tri(1, 1, 2, 2, 3, 3, 0x00010101));
    (void)vpool_free(p, mid);                          /* another mid hole  */
    hs[7] = vpool_alloc(p, tris[7] = mk_tri(36, 14, 48, 16, 40, 28, 0x008040C0));

    /* reallocate into the recycled logical slot 3; a grow must carry both
       its new generation and every other slot's generation verbatim */
    hs[3] = vpool_alloc(p, tris[3] = mk_tri(8, 26, 24, 30, 12, 44, 0x00EEEE30));
    hs[8] = vpool_alloc(p, tris[8] = mk_tri(54, 2, 62, 4, 58, 12, 0x0040C080));
    Handle tail = vpool_alloc(p, mk_tri(9, 9, 10, 10, 11, 11, 0x02020202));
    (void)vpool_free(p, tail);
    hs[9] = vpool_alloc(p, tris[9] = mk_tri(2, 34, 16, 38, 8, 46, 0x00AAAAAA));
    hs[10] = vpool_alloc(p, tris[10] = mk_tri(44, 36, 60, 40, 50, 46, 0x00555555));

    /* live update through a handle handed out before any grow */
    tris[1] = mk_tri(20, 6, 34, 8, 26, 22, 0x0010EE10);
    check("update_early_handle", vpool_update(p, hs[1], tris[1]) == 0);
}

static void render_all(const VPool* p, const Handle* hs, Frame* f) {
    frame_clear(f);
    for (int i = 0; i < N_TRI; i++) {
        if (vpool_submit(p, hs[i], f) != 0) {
            check("submit_live", 0);
        }
    }
}

int main(void) {
    /* ---- basics on a pool that never grows ---- */
    VPool a;
    if (vpool_init(&a, 16) != 0) { printf("init failed\n"); return 1; }
    Handle h0 = vpool_alloc(&a, mk_tri(1, 1, 5, 1, 3, 6, 0x00FFFFFF));
    check("alloc_ok", vpool_valid(&a, h0));
    check("first_gen", h0.gen == 1);

    Tri got;
    check("get_ok", vpool_get(&a, h0, &got) == 0 && got.color == 0x00FFFFFF);

    Handle stale = vpool_alloc(&a, mk_tri(0, 0, 1, 0, 0, 1, 0x00111111));
    check("free_ok", vpool_free(&a, stale) == 0);
    check("stale_rejected", !vpool_valid(&a, stale));
    check("stale_get", vpool_get(&a, stale, &got) == -1);

    Handle reused = vpool_alloc(&a, mk_tri(0, 0, 1, 0, 0, 1, 0x00222222));
    check("reuse_slot", reused.slot == stale.slot);
    check("gen_bumped", reused.gen == stale.gen + 1);
    check("old_stale_still_bad", !vpool_valid(&a, stale));
    check("double_free", vpool_free(&a, reused) == 0
                      && vpool_free(&a, reused) == -1);

    check("malformed_bad", !vpool_valid(&a, BAD_HANDLE));
    Handle oob = { 9999u, 1u };
    check("malformed_oob", !vpool_valid(&a, oob));
    Handle gen0 = { 0u, 0u };
    check("malformed_gen0", !vpool_valid(&a, gen0));
    vpool_destroy(&a);

    /* ---- growable pool vs. never-growing pool, same scene ---- */
    VPool tiny, big;
    if (vpool_init(&tiny, 2) != 0 || vpool_init(&big, 32) != 0) {
        printf("init failed\n"); return 1;
    }

    /* handle handed out before ANY grow; must remain valid afterwards */
    Tri early_tri = mk_tri(30, 18, 38, 20, 34, 30, 0x00DEAD00);
    Handle early = vpool_alloc(&tiny, early_tri);
    check("early_before_grow", tiny.grows == 0 && vpool_valid(&tiny, early));

    Handle ht[N_TRI], hb[N_TRI];
    Tri tris[N_TRI];
    build_scene(&tiny, ht, tris);
    build_scene(&big, hb, tris);

    check("tiny_grew_three_plus", tiny.grows >= 3);
    check("big_never_grew", big.grows == 0);
    check("counts_match", tiny.used == N_TRI + 1 && big.used == N_TRI);
    check("tiny_cap_doubled", tiny.cap == 2 << tiny.grows);
    check("big_cap_untouched", big.cap == 32 && big.grows == 0);

    /* generation bits were not reset: every live handle still validates */
    int all_live = 1;
    for (int i = 0; i < N_TRI; i++) {
        if (!vpool_valid(&tiny, ht[i]) || !vpool_valid(&big, hb[i]))
            all_live = 0;
    }
    check("handles_survive_grows", all_live && vpool_valid(&tiny, early));

    /* the pre-grow handle still points at its own vertex data, untouched */
    Tri early_got;
    check("early_handle_same_data",
          vpool_get(&tiny, early, &early_got) == 0 &&
          memcmp(&early_got, &early_tri, sizeof(Tri)) == 0);

    /* each handle resolves to identical vertex data in both pools */
    int same_data = 1;
    for (int i = 0; i < N_TRI; i++) {
        Tri tt, tb;
        if (vpool_get(&tiny, ht[i], &tt) != 0 ||
            vpool_get(&big, hb[i], &tb) != 0 ||
            memcmp(&tt, &tb, sizeof(Tri)) != 0)
            same_data = 0;
    }
    check("vertex_data_identical", same_data);

    /* index map rebuild under grow, observed step by step. */
    VPool watch;
    if (vpool_init(&watch, 4) != 0) { printf("init failed\n"); return 1; }
    Handle w0 = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0001));
    Handle w1 = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0002));
    Handle w2 = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0003));
    Handle w3 = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0004));

    /* free a middle logical slot: free compaction moves the last live
       triangle (w3, physical 3) into the hole (physical 1); w2 is
       untouched at physical 2. Its map entry follows the data. */
    int phys2_pre = watch.map[w2.slot];
    int phys3_pre = watch.map[w3.slot];
    (void)vpool_free(&watch, w1);
    check("free_compact_moved_data",
          watch.map[w2.slot] == phys2_pre &&
          watch.map[w3.slot] == 1 && phys3_pre == 3);
    Tri tw;
    check("free_compact_keeps_data",
          vpool_get(&watch, w3, &tw) == 0 && tw.color == 0x00AA0004 &&
          vpool_get(&watch, w2, &tw) == 0 && tw.color == 0x00AA0003);

    /* recycle the freed logical slot (new generation), then force a grow */
    Handle w1r = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0006));
    check("recycled_slot", w1r.slot == w1.slot && w1r.gen == w1.gen + 1);
    check("watch_full_again", watch.used == watch.cap);

    int phys_pre[4] = {
        watch.map[w0.slot], watch.map[w1r.slot],
        watch.map[w2.slot],  watch.map[w3.slot]
    };
    /* snapshot which vertex data sat at each physical slot pre-grow */
    Tri pre_phys[4];
    for (int i = 0; i < 4; i++) pre_phys[i] = watch.arena[i];
    int grows_before = watch.grows;
    Handle extra = vpool_alloc(&watch, mk_tri(0, 0, 1, 0, 0, 1, 0x00AA0008));
    check("watch_grew", watch.grows == grows_before + 1 && watch.cap == 8);

    /* grow compaction re-packs in logical-slot order, so physical positions
       may change; what must hold is that each handle's map entry points at
       a physical slot holding exactly that handle's pre-grow vertex data */
    Handle wh[4] = { w0, w1r, w2, w3 };
    int grow_map_ok = 1;
    for (int i = 0; i < 4; i++) {
        int src = phys_pre[i];
        int dst = watch.map[wh[i].slot];
        if (dst < 0 ||
            memcmp(&watch.arena[dst], &pre_phys[src], sizeof(Tri)) != 0)
            grow_map_ok = 0;
    }
    check("grow_map_moves_data", grow_map_ok);
    /* and compaction demonstrably relocated something */
    int relocated = 0;
    for (int i = 0; i < 4; i++)
        if (watch.map[wh[i].slot] != phys_pre[i]) relocated = 1;
    check("grow_relocated_slots", relocated);

    int watch_ok = 1;
    if (vpool_get(&watch, w0,  &tw) != 0 || tw.color != 0x00AA0001) watch_ok = 0;
    if (vpool_get(&watch, w1r, &tw) != 0 || tw.color != 0x00AA0006) watch_ok = 0;
    if (vpool_get(&watch, w2,  &tw) != 0 || tw.color != 0x00AA0003) watch_ok = 0;
    if (vpool_get(&watch, w3,  &tw) != 0 || tw.color != 0x00AA0004) watch_ok = 0;
    if (vpool_get(&watch, extra, &tw) != 0 || tw.color != 0x00AA0008) watch_ok = 0;
    check("watch_data_intact", watch_ok);

    /* arena remains packed: every physical slot 0..used-1 claimed once */
    int claimed[8] = {0};
    for (int i = 0; i < watch.cap; i++)
        if (watch.in_use[i]) claimed[watch.map[i]]++;
    int packed = 1;
    for (int i = 0; i < watch.used; i++)
        if (claimed[i] != 1) packed = 0;
    check("watch_packed", packed && watch.used == 5);

    /* freed generation was carried through the grow, not reset */
    check("watch_stale_stays_stale", !vpool_valid(&watch, w1));
    vpool_destroy(&watch);

    /* map stays dense and bijective over live slots */
    int map_ok = 1;
    for (int i = 0; i < tiny.cap; i++) {
        if (tiny.in_use[i]) {
            if (tiny.map[i] < 0 || tiny.map[i] >= tiny.used) map_ok = 0;
        } else if (tiny.map[i] != -1) {
            map_ok = 0;
        }
    }
    check("map_dense", map_ok);

    /* pixel-for-pixel: grown pool renders exactly what the non-grown pool
       renders, and the frame is independent of grow/compaction history */
    Frame ft, fb;
    render_all(&tiny, ht, &ft);
    render_all(&big, hb, &fb);
    int pixels_same = memcmp(ft.px, fb.px, sizeof(ft.px)) == 0;
    check("pixel_identical", pixels_same);

    /* non-empty frame so the comparison is meaningful */
    int painted = 0;
    for (int i = 0; i < FW * FH; i++) if (ft.px[i] != BG) painted++;
    check("frame_has_pixels", painted > 50);

    /* three consecutive grows: keep allocating, assert data after each */
    int g0 = tiny.grows;
    int triple_ok = 1;
    Handle wave[512];
    int nw = 0;
    while (tiny.grows < g0 + 3 && nw < 512) {
        Tri w = mk_tri(nw % FW, 0, (nw + 3) % FW, 2, (nw + 1) % FW, 4,
                       0x00000000u | (unsigned)(0x100 + nw));
        wave[nw] = vpool_alloc(&tiny, w);
        if (!vpool_valid(&tiny, wave[nw])) { triple_ok = 0; break; }
        nw++;
    }
    for (int i = 0; i < nw; i++) {
        Tri w;
        if (vpool_get(&tiny, wave[i], &w) != 0 ||
            w.color != (unsigned)(0x100 + i))
            triple_ok = 0;
    }
    check("three_grows_stable", triple_ok && tiny.grows >= g0 + 3);

    /* stale handle from THIS pool: submit fails and paints nothing */
    Handle local_stale = vpool_alloc(&tiny,
        mk_tri(20, 20, 24, 20, 22, 26, 0x00777777));
    check("local_stale_live_once", vpool_valid(&tiny, local_stale));
    (void)vpool_free(&tiny, local_stale);
    Frame f1, f2;
    frame_clear(&f1);
    frame_clear(&f2);
    check("stale_submit_fails",
          vpool_submit(&tiny, local_stale, &f1) == -1);
    check("stale_submit_no_pixels",
          memcmp(f1.px, f2.px, sizeof(f1.px)) == 0);

    /* stale-from-free handle stays stale even after further grows */
    Handle old = vpool_alloc(&tiny, mk_tri(3, 3, 9, 3, 6, 9, 0x00ABCDEF));
    check("old_live_once", vpool_valid(&tiny, old));
    (void)vpool_free(&tiny, old);
    int grows_before_burst = tiny.grows;
    int burst_ok = 1;
    for (int burst = 0; burst < 300; burst++) {
        Handle filler = vpool_alloc(&tiny,
            mk_tri(0, 40, 4, 40, 2, 44, 0x000F0F0F));
        if (!vpool_valid(&tiny, filler)) { burst_ok = 0; break; }
    }
    check("burst_alloc_ok", burst_ok);
    check("burst_forced_grows", tiny.grows > grows_before_burst);
    check("stale_after_more_grows", !vpool_valid(&tiny, old));
    check("stale_after_grow_submit", vpool_submit(&tiny, old, &f1) == -1);
    check("stale_after_grow_no_pixels",
          memcmp(f1.px, f2.px, sizeof(f1.px)) == 0);

    /* free through a handle obtained pre-grow; its slot must be found via
       the rebuilt map, and afterwards the handle must be stale */
    int used_before = tiny.used;
    check("free_pre_grow_handle", vpool_free(&tiny, ht[5]) == 0);
    check("freed_handle_stale", !vpool_valid(&tiny, ht[5]));
    check("used_decremented", tiny.used == used_before - 1);
    check("free_freed_again", vpool_free(&tiny, ht[5]) == -1);

    vpool_destroy(&tiny);
    vpool_destroy(&big);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
