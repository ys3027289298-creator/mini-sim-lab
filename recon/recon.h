/* recon: cross-module, frame-aligned reconciliation ledger.
 *
 * The ledger is opt-in at compile time.  Without -DRECON every instrumentation
 * call expands to a no-op macro, so the modules compile to exactly the code
 * they had before and their self-test output is unchanged.
 *
 * Every module reports each metric twice:
 *   - recorded: the ledger observes the event itself (one increment per step,
 *     allocation, free, or random draw);
 *   - expected: at the end of each frame the module checkpoints its own
 *     cumulative internal counter, and the ledger turns it into a per-frame
 *     delta.
 * The two views are compared cell by cell so a disagreement is reported as
 * `frame=<n> item=<name>`.
 */
#ifndef RECON_H
#define RECON_H

#include <stdint.h>

#define RECON_MAX_FRAMES 256u
#define RECON_VERSION 1u

#define RECON_FILE_MAGIC 0x52454354u /* "RECT" */
#define RECON_ROW_MAGIC  0x524f5752u /* "ROWR" */

/* Items are listed in table-column order.  The string is the stable name used
 * by diagnostics, fault injection and tests. */
#define RECON_ITEM_LIST(X)          \
    X(FS_STEPS, "fs.steps")         \
    X(FS_DROPPED, "fs.dropped")     \
    X(POOL_ALLOCS, "pool.allocs")   \
    X(POOL_FREES, "pool.frees")     \
    X(POOL_FAILED, "pool.failed")   \
    X(POOL_USED, "pool.used")       \
    X(RNG_DRAWS, "rng.draws")       \
    X(RNG_RANGES, "rng.ranges")

typedef enum {
#define RECON_ENUM_ITEM(name, text) RECON_ITEM_##name,
    RECON_ITEM_LIST(RECON_ENUM_ITEM)
#undef RECON_ENUM_ITEM
    RECON_ITEM_COUNT
} ReconItem;

/* COUNT items are event increments summed over a frame; GAUGE items are a
 * level sampled at the end of a frame.  The kind is part of each item's
 * counting convention. */
typedef enum {
    RECON_KIND_COUNT = 0,
    RECON_KIND_GAUGE = 1
} ReconKind;

#define RECON_KIND_FS_STEPS    RECON_KIND_COUNT
#define RECON_KIND_FS_DROPPED  RECON_KIND_COUNT
#define RECON_KIND_POOL_ALLOCS RECON_KIND_COUNT
#define RECON_KIND_POOL_FREES  RECON_KIND_COUNT
#define RECON_KIND_POOL_FAILED RECON_KIND_COUNT
#define RECON_KIND_POOL_USED   RECON_KIND_GAUGE
#define RECON_KIND_RNG_DRAWS   RECON_KIND_COUNT
#define RECON_KIND_RNG_RANGES  RECON_KIND_COUNT

#ifdef RECON

#include <stdio.h>

typedef struct {
    uint32_t magic;
    uint32_t frame;
    int64_t recorded[RECON_ITEM_COUNT]; /* observed by the ledger */
    int64_t expected[RECON_ITEM_COUNT]; /* module checkpoints, per frame */
    uint32_t have;    /* bit i: expected[i] was checkpointed */
    uint32_t rec_set; /* bit i: recorded[i] was observed */
    uint32_t checksum;
} ReconRow;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t count;          /* completed frames */
    uint32_t current_frame;
    uint32_t flags;
    uint32_t sealed_frames;  /* expected frame count once sealed */
    uint32_t error_count;
    ReconRow rows[RECON_MAX_FRAMES];
} ReconLedger;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t row_size;
    uint32_t capacity;
    uint32_t count;
    uint32_t sealed_frames;
    uint32_t checksum;
} ReconFileHeader;

typedef struct {
    uint32_t magic;
    uint32_t ledger_checksum;
} ReconFileTrailer;

enum {
    RECON_LEDGER_OPEN   = 1u << 0,
    RECON_LEDGER_SEALED = 1u << 1
};

/* ---- lifecycle ---------------------------------------------------------- */

void recon_init(ReconLedger* ledger);

/* Modules log through the active ledger.  With no active ledger every
 * instrumentation call is a no-op, so module self-tests are unaffected. */
void recon_set_active(ReconLedger* ledger);
ReconLedger* recon_get_active(void);
void recon_set_log(FILE* out);

/* ---- frame lifecycle ---------------------------------------------------- */

/* Frame ids must be contiguous from 0: frame == ledger->count. */
int recon_frame_begin(ReconLedger* ledger, uint32_t frame);
int recon_frame_end(ReconLedger* ledger);

/* Declares how many complete rows the table must contain and validates
 * structure, completeness and every cell.  Returns the problem count. */
int recon_seal(ReconLedger* ledger, uint32_t frames);

/* ---- instrumentation (called by modules) -------------------------------- */

void recon_log_count(ReconItem item, int64_t increment);
void recon_log_gauge(ReconItem item, int64_t value);
void recon_checkpoint_count(ReconItem item, int64_t cumulative);
void recon_checkpoint_gauge(ReconItem item, int64_t value);

/* ---- inspection / validation ------------------------------------------- */

const char* recon_item_name(ReconItem item);
ReconKind recon_item_kind(ReconItem item);
int recon_item_from_name(const char* name);
int64_t recon_total(const ReconLedger* ledger, ReconItem item);

/* Validates structure (magic/version/row checksums/frame continuity/
 * completeness) and every cell.  Prints diagnostics naming the frame and
 * item.  Returns the number of problems found. */
int recon_verify(const ReconLedger* ledger, FILE* out);
void recon_print_table(const ReconLedger* ledger, FILE* out);

/* Persist/restore the table.  Loading rejects corrupt tables and incomplete
 * tables (missing rows or trailer):
 *    0  ok
 *   -2  incomplete
 *   -4  malformed header/format
 *   -5  row, header or ledger checksum mismatch
 */
int recon_save(const ReconLedger* ledger, const char* path);
int recon_load(ReconLedger* ledger, const char* path);

/* Test-only fault injection.  Rewrites one cell and re-seals that row's
 * checksum: this simulates a counter disagreement, not a corrupt file. */
int recon_inject_value_fault(ReconLedger* ledger, uint32_t frame,
                             ReconItem item, int64_t delta);
/* Test-only: drop one checkpoint, simulating an incomplete table. */
int recon_inject_missing(ReconLedger* ledger, uint32_t frame, ReconItem item);

#else /* !RECON */

/* With reconciliation disabled the modules still contain the calls, but they
 * evaporate before code generation.  Keeping the item enum above means call
 * sites do not need their own #ifdefs.  recon.c itself always compiles with
 * RECON defined; these macros are for the modules only. */
#define recon_init(ledger)                     ((void)(ledger))
#define recon_set_active(ledger)               ((void)(ledger))
#define recon_get_active()                     ((void*)0)
#define recon_set_log(out)                     ((void)(out))
#define recon_frame_begin(ledger, frame)       ((void)(ledger), (void)(frame), 0)
#define recon_frame_end(ledger)                ((void)(ledger), 0)
#define recon_seal(ledger, frames)             ((void)(ledger), (void)(frames), 0)
#define recon_log_count(item, increment)        ((void)(item), (void)(increment))
#define recon_log_gauge(item, value)            ((void)(item), (void)(value))
#define recon_checkpoint_count(item, cumulative) ((void)(item), (void)(cumulative))
#define recon_checkpoint_gauge(item, value)     ((void)(item), (void)(value))
#define recon_item_name(item)                   ((void)(item), "")
#define recon_item_from_name(name)              ((void)(name), -1)
#define recon_total(ledger, item)               ((void)(ledger), (void)(item), 0)
#define recon_verify(ledger, out)               ((void)(ledger), (void)(out), 0)
#define recon_print_table(ledger, out)          ((void)(ledger), (void)(out))
#define recon_save(ledger, path)                ((void)(ledger), (void)(path), -1)
#define recon_load(ledger, path)                ((void)(ledger), (void)(path), -1)
#define recon_inject_value_fault(l, f, i, d)    ((void)(l), (void)(f), (void)(i), (void)(d), -1)
#define recon_inject_missing(l, f, i)           ((void)(l), (void)(f), (void)(i), -1)

#endif /* RECON */

#endif /* RECON_H */
