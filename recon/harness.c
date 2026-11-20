/* harness: drives fixedstep, pool and rng through the same frames, records a
 * single reconciliation table, and checks it against each module's own books.
 *
 * The three modules are compiled into this translation unit with their
 * self-tests disabled, so the harness can call their (static) internals. */
#define _POSIX_C_SOURCE 200809L
#define RECON 1
#define RECON_NO_SELFTEST 1

#include <stdlib.h>
#include <string.h>

#include "recon.h"

#include "../fixedstep/src/fixedstep.c"
#include "../pool/src/pool.c"
#include "../rng/src/rng.c"

#define FRAMES 24

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ---- deterministic per-frame workload ---------------------------------- */

typedef struct {
    Clock clock;
    Pool pool;
    Stream rng;
    Handle handles[POOL_CAP];
    int handle_count;
} World;

static void world_init(World* w) {
    memset(w, 0, sizeof *w);
    clock_init(&w->clock, 0.01, 3);
    pool_init(&w->pool);
    stream_init(&w->rng, 0xC0FFEE);
}

/* frame_dt table: mostly one step, sometimes a fraction, and on two frames a
 * backlog large enough that the catch-up cap discards steps. */
static double frame_dt_for(uint32_t f) {
    if (f == 5) return 0.085;   /* 8.5 steps of backlog, cap 3 -> drop */
    if (f == 11) return 0.055;  /* 5.5 steps, cap 3 -> drop */
    if (f % 3 == 0) return 0.005;
    if (f % 3 == 1) return 0.02;
    return 0.01;
}

static void drive_frame(World* w, uint32_t f) {
    /* fixedstep */
    clock_advance(&w->clock, frame_dt_for(f));

    /* pool: allocate a few each frame, free some of them, and on one frame
     * try to over-allocate so a failure is recorded too. */
    int allocs = 1 + (int)(f % 3);
    for (int i = 0; i < allocs; i++) {
        Handle h = pool_alloc(&w->pool, (int)f * 10 + i);
        if (h.index != (unsigned)-1 && w->handle_count < POOL_CAP)
            w->handles[w->handle_count++] = h;
    }
    if (f == 9) {
        for (int i = 0; i < 40; i++) (void)pool_alloc(&w->pool, 999);
    }
    int frees = (int)(f % 2);
    for (int i = 0; i < frees && w->handle_count > 0; i++) {
        Handle h = w->handles[--w->handle_count];
        pool_free(&w->pool, h);
    }

    /* rng: a couple of raw draws and a range draw every frame */
    (void)stream_next(&w->rng);
    if (f % 2 == 0) (void)stream_next(&w->rng);
    (void)stream_range(&w->rng, 6);
}

/* End-of-frame checkpoint: every module reports its own cumulative books. */
static void report_frame(World* w) {
    clock_recon_frame(&w->clock);
    pool_recon_frame(&w->pool);
    stream_recon_frame(&w->rng);
}

/* ---- one full run ------------------------------------------------------ */

static int run_ledger(ReconLedger* ledger, World* w) {
    recon_init(ledger);
    world_init(w);
    recon_set_active(ledger);
    for (uint32_t f = 0; f < FRAMES; f++) {
        if (recon_frame_begin(ledger, f) != 0) return -1;
        drive_frame(w, f);
        report_frame(w);
        if (recon_frame_end(ledger) != 0) return -1;
    }
    recon_set_active(NULL);
    return recon_seal(ledger, FRAMES);
}

/* Copy `src` to `dst`, optionally truncating to `keep` bytes. */
static int copy_prefix(const char* src, const char* dst, long keep) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    long written = 0;
    int ch;
    while ((ch = fgetc(in)) != EOF && (keep < 0 || written < keep)) {
        fputc(ch, out);
        written++;
    }
    fclose(in);
    fclose(out);
    return 0;
}

static void read_text(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
}

int main(void) {
    char dir[] = "/tmp/recon-harness-XXXXXX";
    if (!mkdtemp(dir)) { printf("temp_dir=0\n"); return 1; }
    char path[256];
    snprintf(path, sizeof path, "%s/table.rct", dir);

    ReconLedger ledger;
    World w;

    /* --- happy path: every cell agrees -------------------------------- */
    int sealed_issues = run_ledger(&ledger, &w);
    check("seal_clean", sealed_issues == 0);

    char diag[256];
    snprintf(diag, sizeof diag, "%s/diag.txt", dir);
    FILE* verify_out = fopen(diag, "wb+");
    int issues = verify_out ? recon_verify(&ledger, verify_out) : -1;
    check("verify_clean", issues == 0);
    char text[1024] = {0};
    if (verify_out) {
        fclose(verify_out);
        read_text(diag, text, sizeof text);
    }
    check("verify_no_diagnostics", text[0] == '\0');

    /* The table totals must equal each module's own counter. */
    check("total_fs_steps_matches_clock",
          recon_total(&ledger, RECON_ITEM_FS_STEPS) == w.clock.total_steps);
    check("total_fs_dropped_matches_clock",
          recon_total(&ledger, RECON_ITEM_FS_DROPPED) == w.clock.dropped);
    check("total_pool_allocs_matches_pool",
          recon_total(&ledger, RECON_ITEM_POOL_ALLOCS) == w.pool.alloc_count);
    check("total_pool_frees_matches_pool",
          recon_total(&ledger, RECON_ITEM_POOL_FREES) == w.pool.free_count);
    check("total_pool_failed_matches_pool",
          recon_total(&ledger, RECON_ITEM_POOL_FAILED) == w.pool.fail_count);
    check("gauge_pool_used_matches_pool",
          recon_total(&ledger, RECON_ITEM_POOL_USED) == w.pool.used);
    check("total_rng_draws_matches_stream",
          recon_total(&ledger, RECON_ITEM_RNG_DRAWS) == w.rng.draw_count);
    check("total_rng_ranges_matches_stream",
          recon_total(&ledger, RECON_ITEM_RNG_RANGES) == w.rng.range_count);

    /* The workload actually exercised the interesting paths. */
    check("workload_dropped_steps", w.clock.dropped > 0);
    check("workload_pool_failure", w.pool.fail_count > 0);
    check("workload_frames", ledger.count == FRAMES);

    recon_print_table(&ledger, stdout);

    /* --- a wrong cell names the frame and the item --------------------- */
    ReconLedger faulty;
    World fw;
    run_ledger(&faulty, &fw);
    recon_inject_value_fault(&faulty, 4, RECON_ITEM_POOL_ALLOCS, 2);
    FILE* bad_out = fopen(diag, "wb+");
    int fault_issues = bad_out ? recon_verify(&faulty, bad_out) : -1;
    if (bad_out) fclose(bad_out);
    char fault_text[1024] = {0};
    read_text(diag, fault_text, sizeof fault_text);
    check("cell_fault_detected", fault_issues > 0);
    check("cell_fault_names_frame_and_item",
          strstr(fault_text, "frame=4 item=pool.allocs") != NULL);

    /* A damaged table is reported as damaged, not as a bad cell. */
    recon_inject_value_fault(&faulty, 4, RECON_ITEM_POOL_ALLOCS, -2);
    faulty.rows[4].checksum ^= 0xFFFFFFFFu;
    FILE* corrupt_out = fopen(diag, "wb+");
    int corrupt_issues = corrupt_out ? recon_verify(&faulty, corrupt_out) : -1;
    if (corrupt_out) fclose(corrupt_out);
    char corrupt_text[1024] = {0};
    read_text(diag, corrupt_text, sizeof corrupt_text);
    check("row_checksum_detected", corrupt_issues > 0);
    check("row_checksum_names_frame",
          strstr(corrupt_text, "error: frame=4 row checksum mismatch") != NULL);

    /* --- missing checkpoint is an incomplete table -------------------- */
    ReconLedger missing;
    World mw;
    run_ledger(&missing, &mw);
    recon_inject_missing(&missing, 7, RECON_ITEM_RNG_RANGES);
    FILE* miss_out = fopen(diag, "wb+");
    int miss_issues = miss_out ? recon_verify(&missing, miss_out) : -1;
    if (miss_out) fclose(miss_out);
    char miss_text[1024] = {0};
    read_text(diag, miss_text, sizeof miss_text);
    check("missing_cell_detected", miss_issues > 0);
    check("missing_cell_names_frame_and_item",
          strstr(miss_text, "frame=7 item=rng.ranges") != NULL);

    /* --- persistence round-trip --------------------------------------- */
    ReconLedger loaded;
    check("save_ok", recon_save(&ledger, path) == 0);
    check("load_ok", recon_load(&loaded, path) == 0);
    check("roundtrip_rows", loaded.count == ledger.count);
    check("roundtrip_checksum",
          loaded.rows[FRAMES - 1].checksum == ledger.rows[FRAMES - 1].checksum);
    FILE* rt_out = fopen(diag, "wb+");
    int rt_issues = rt_out ? recon_verify(&loaded, rt_out) : -1;
    if (rt_out) fclose(rt_out);
    check("roundtrip_verifies", rt_issues == 0);

    /* --- incomplete file: truncated before the trailer ---------------- */
    long full = 0;
    FILE* size_f = fopen(path, "rb");
    if (size_f) { fseek(size_f, 0, SEEK_END); full = ftell(size_f); fclose(size_f); }
    check("save_has_bytes", full > 0);
    char trunc[256];
    snprintf(trunc, sizeof trunc, "%s/trunc.rct", dir);
    check("truncate_ok", copy_prefix(path, trunc, full - 4) == 0);
    check("load_truncated_incomplete",
          recon_load(&loaded, trunc) == -2);

    /* --- incomplete file: one full row short -------------------------- */
    char shortpath[256];
    snprintf(shortpath, sizeof shortpath, "%s/short.rct", dir);
    long one_row = (long)sizeof(ReconFileHeader) +
                   (long)(sizeof(ReconRow) * (FRAMES - 1));
    check("shorten_ok", copy_prefix(path, shortpath, one_row) == 0);
    check("load_short_incomplete", recon_load(&loaded, shortpath) == -2);

    /* --- corrupt file: flipped byte inside a row ---------------------- */
    char corruptpath[256];
    snprintf(corruptpath, sizeof corruptpath, "%s/corrupt.rct", dir);
    check("copy_ok", copy_prefix(path, corruptpath, -1) == 0);
    FILE* cf = fopen(corruptpath, "r+b");
    long row_off = (long)sizeof(ReconFileHeader) +
                   (long)(sizeof(ReconRow) * 2) + 12;
    if (cf) {
        fseek(cf, row_off, SEEK_SET);
        int c = fgetc(cf);
        fseek(cf, row_off, SEEK_SET);
        fputc(c ^ 0xFF, cf);
        fclose(cf);
    }
    check("load_corrupt_rejected", recon_load(&loaded, corruptpath) == -5);

    /* --- corrupt file: bad header magic ------------------------------- */
    char hdrpath[256];
    snprintf(hdrpath, sizeof hdrpath, "%s/hdr.rct", dir);
    copy_prefix(path, hdrpath, -1);
    FILE* hf = fopen(hdrpath, "r+b");
    if (hf) { fputc(0x00, hf); fclose(hf); }
    check("load_bad_header_rejected", recon_load(&loaded, hdrpath) == -4);

    /* --- ledger short of its declared frame count --------------------- */
    ReconLedger partial;
    World pw;
    recon_init(&partial);
    world_init(&pw);
    recon_set_active(&partial);
    for (uint32_t f = 0; f < FRAMES - 2; f++) {
        recon_frame_begin(&partial, f);
        drive_frame(&pw, f);
        report_frame(&pw);
        recon_frame_end(&partial);
    }
    recon_set_active(NULL);
    int under = recon_seal(&partial, FRAMES);
    check("incomplete_ledger_seal_fails", under > 0);
    FILE* under_out = fopen(diag, "wb+");
    int under_issues = under_out ? recon_verify(&partial, under_out) : -1;
    if (under_out) fclose(under_out);
    char under_text[1024] = {0};
    read_text(diag, under_text, sizeof under_text);
    check("incomplete_ledger_verify_fails", under_issues > 0);
    check("incomplete_ledger_names_counts",
          strstr(under_text, "incomplete: seal expects 24 frames, have 22") != NULL);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
