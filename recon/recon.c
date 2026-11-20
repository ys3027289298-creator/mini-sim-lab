/* recon: cross-module, frame-aligned reconciliation ledger. */
#define RECON 1
#include "recon.h"

#include <stdarg.h>
#include <string.h>

static const char* const kItemNames[RECON_ITEM_COUNT] = {
#define RECON_NAME_ENTRY(name, text) text,
    RECON_ITEM_LIST(RECON_NAME_ENTRY)
#undef RECON_NAME_ENTRY
};

static const ReconKind kItemKinds[RECON_ITEM_COUNT] = {
#define RECON_KIND_ENTRY(name, text) RECON_KIND_##name,
    RECON_ITEM_LIST(RECON_KIND_ENTRY)
#undef RECON_KIND_ENTRY
};

/* Last cumulative value checkpointed for each item.  Turning a module's
 * running counter into a per-frame delta happens here, so the modules never
 * need to remember what the previous frame reported. */
static int64_t g_cumulative[RECON_ITEM_COUNT];

static ReconLedger* g_active = NULL;
static FILE* g_log = NULL;

static const uint32_t kTrailerMagic = 0xA5A55A5Au;

/* ---- names and kinds ---------------------------------------------------- */

const char* recon_item_name(ReconItem item) {
    if ((unsigned)item >= RECON_ITEM_COUNT) return "<bad-item>";
    return kItemNames[item];
}

ReconKind recon_item_kind(ReconItem item) {
    if ((unsigned)item >= RECON_ITEM_COUNT) return RECON_KIND_COUNT;
    return kItemKinds[item];
}

int recon_item_from_name(const char* name) {
    if (!name) return -1;
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
        if (strcmp(kItemNames[i], name) == 0) return (int)i;
    return -1;
}

/* ---- checksums ---------------------------------------------------------- */

static uint64_t hash_bytes(uint64_t h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t hash_u32(uint64_t h, uint32_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
    return hash_bytes(h, b, sizeof b);
}

static uint64_t hash_i64(uint64_t h, int64_t v) {
    uint64_t u = (uint64_t)v;
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (8 * i));
    return hash_bytes(h, b, sizeof b);
}

static uint64_t hash_begin(void) { return 0xCBF29CE484222325ULL; }

static uint32_t row_checksum(const ReconRow* row) {
    uint64_t h = hash_begin();
    h = hash_u32(h, row->magic);
    h = hash_u32(h, row->frame);
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
        h = hash_i64(h, row->recorded[i]);
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
        h = hash_i64(h, row->expected[i]);
    h = hash_u32(h, row->have);
    h = hash_u32(h, row->rec_set);
    return (uint32_t)h;
}

static uint32_t ledger_checksum(const ReconLedger* ledger) {
    uint64_t h = hash_begin();
    h = hash_u32(h, ledger->version);
    h = hash_u32(h, ledger->capacity);
    h = hash_u32(h, ledger->count);
    h = hash_u32(h, ledger->sealed_frames);
    for (uint32_t i = 0; i < ledger->count; i++)
        h = hash_u32(h, ledger->rows[i].checksum);
    return (uint32_t)h;
}

static uint32_t header_checksum(const ReconFileHeader* hdr) {
    uint64_t h = hash_begin();
    h = hash_u32(h, hdr->magic);
    h = hash_u32(h, hdr->version);
    h = hash_u32(h, hdr->row_size);
    h = hash_u32(h, hdr->capacity);
    h = hash_u32(h, hdr->count);
    h = hash_u32(h, hdr->sealed_frames);
    return (uint32_t)h;
}

/* ---- logging ------------------------------------------------------------ */

/* Diagnostics go to the optional log sink.  Tests that need the text use
 * recon_verify(), which returns it through an explicit FILE*. */
static void log_line(const char* line) {
    if (!g_log) return;
    fputs(line, g_log);
    fflush(g_log);
}

/* ---- lifecycle ---------------------------------------------------------- */

static void recon_reset_counters(void) {
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++) g_cumulative[i] = 0;
}

void recon_init(ReconLedger* ledger) {
    if (!ledger) return;
    memset(ledger, 0, sizeof *ledger);
    ledger->magic = RECON_FILE_MAGIC;
    ledger->version = RECON_VERSION;
    ledger->capacity = RECON_MAX_FRAMES;
    ledger->flags = RECON_LEDGER_OPEN;
    for (uint32_t i = 0; i < RECON_MAX_FRAMES; i++)
        ledger->rows[i].magic = RECON_ROW_MAGIC;
    recon_reset_counters();
}

void recon_set_active(ReconLedger* ledger) {
    g_active = ledger;
    if (ledger) recon_reset_counters();
}

ReconLedger* recon_get_active(void) { return g_active; }

void recon_set_log(FILE* out) { g_log = out; }

/* ---- frame lifecycle ---------------------------------------------------- */

int recon_frame_begin(ReconLedger* ledger, uint32_t frame) {
    if (!ledger) return -1;
    if (!(ledger->flags & RECON_LEDGER_OPEN)) return -2;
    if (ledger->count >= ledger->capacity) return -3;
    if (frame != ledger->count) return -4;
    ReconRow* row = &ledger->rows[ledger->count];
    memset(row, 0, sizeof *row);
    row->magic = RECON_ROW_MAGIC;
    row->frame = frame;
    ledger->current_frame = frame;
    return 0;
}

/* Compare one row's cells.  `out` receives the same text as the log. */
static int validate_cells(const ReconRow* row, FILE* out) {
    int bad = 0;
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++) {
        if (!(row->have & (1u << i))) continue; /* completeness is seal's job */
        int64_t rec = row->recorded[i];
        int64_t exp = row->expected[i];
        if (rec == exp) continue;
        bad++;
        char buf[192];
        snprintf(buf, sizeof buf,
                 "frame=%u item=%s recorded=%lld expected=%lld delta=%lld\n",
                 row->frame, kItemNames[i], (long long)rec, (long long)exp,
                 (long long)(rec - exp));
        log_line(buf);
        if (out) fputs(buf, out);
    }
    return bad;
}

int recon_frame_end(ReconLedger* ledger) {
    if (!ledger) return -1;
    if (!(ledger->flags & RECON_LEDGER_OPEN)) return -2;
    if (ledger->count >= ledger->capacity) return -3;
    ReconRow* row = &ledger->rows[ledger->count];
    int bad = validate_cells(row, NULL);
    ledger->error_count += (uint32_t)bad;
    row->checksum = row_checksum(row);
    ledger->count++;
    return bad;
}

/* Structure and completeness checks shared by seal and verify.  `out` is
 * NULL for seal (diagnostics go to the log only). */
static int check_row_structure(const ReconRow* row, uint32_t expected_frame,
                               FILE* out) {
    int issues = 0;
    char buf[192];
    if (row->magic != RECON_ROW_MAGIC) {
        snprintf(buf, sizeof buf, "error: frame=%u row magic mismatch\n",
                 expected_frame);
        log_line(buf);
        if (out) fputs(buf, out);
        return 1;
    }
    if (row->frame != expected_frame) {
        snprintf(buf, sizeof buf, "error: frame=%u row says frame=%u\n",
                 expected_frame, row->frame);
        log_line(buf);
        if (out) fputs(buf, out);
        return 1;
    }
    if (row->checksum != row_checksum(row)) {
        snprintf(buf, sizeof buf, "error: frame=%u row checksum mismatch\n",
                 expected_frame);
        log_line(buf);
        if (out) fputs(buf, out);
        issues++;
    }
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++) {
        if (!(row->have & (1u << i))) {
            snprintf(buf, sizeof buf,
                     "error: frame=%u item=%s missing checkpoint\n",
                     expected_frame, kItemNames[i]);
            log_line(buf);
            if (out) fputs(buf, out);
            issues++;
        } else if (kItemKinds[i] == RECON_KIND_GAUGE &&
                   !(row->rec_set & (1u << i))) {
            snprintf(buf, sizeof buf,
                     "error: frame=%u item=%s missing recorded value\n",
                     expected_frame, kItemNames[i]);
            log_line(buf);
            if (out) fputs(buf, out);
            issues++;
        }
    }
    return issues;
}

int recon_seal(ReconLedger* ledger, uint32_t frames) {
    if (!ledger) return -1;
    if (frames > ledger->capacity) return -2;
    ledger->sealed_frames = frames;
    ledger->flags |= RECON_LEDGER_SEALED;

    int issues = 0;
    if (ledger->count != frames) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "error: ledger incomplete: seal expects %u frames, have %u\n",
                 frames, ledger->count);
        log_line(buf);
        issues++;
    }
    for (uint32_t f = 0; f < ledger->count; f++) {
        issues += check_row_structure(&ledger->rows[f], f, NULL);
        issues += validate_cells(&ledger->rows[f], NULL);
    }
    ledger->error_count += (uint32_t)issues;
    return issues;
}

/* ---- instrumentation ---------------------------------------------------- */

static int events_enabled(ReconItem item) {
    if (!g_active || (unsigned)item >= RECON_ITEM_COUNT) return 0;
    if (!(g_active->flags & RECON_LEDGER_OPEN)) return 0;
    if (g_active->count >= g_active->capacity) return 0;
    return 1;
}

void recon_log_count(ReconItem item, int64_t increment) {
    if (!events_enabled(item)) return;
    ReconRow* row = &g_active->rows[g_active->count];
    row->recorded[item] += increment;
    row->rec_set |= (1u << item);
}

void recon_log_gauge(ReconItem item, int64_t value) {
    if (!events_enabled(item)) return;
    ReconRow* row = &g_active->rows[g_active->count];
    row->recorded[item] = value;
    row->rec_set |= (1u << item);
}

void recon_checkpoint_count(ReconItem item, int64_t cumulative) {
    if (!events_enabled(item)) return;
    int64_t delta = cumulative - g_cumulative[item];
    g_cumulative[item] = cumulative;
    ReconRow* row = &g_active->rows[g_active->count];
    row->expected[item] += delta;
    row->have |= (1u << item);
}

void recon_checkpoint_gauge(ReconItem item, int64_t value) {
    if (!events_enabled(item)) return;
    g_cumulative[item] = value;
    ReconRow* row = &g_active->rows[g_active->count];
    row->expected[item] = value;
    row->have |= (1u << item);
}

/* ---- inspection / validation ------------------------------------------- */

int64_t recon_total(const ReconLedger* ledger, ReconItem item) {
    if (!ledger || (unsigned)item >= RECON_ITEM_COUNT) return 0;
    int64_t total = 0;
    for (uint32_t f = 0; f < ledger->count; f++) {
        if (kItemKinds[item] == RECON_KIND_GAUGE)
            total = ledger->rows[f].recorded[item];
        else
            total += ledger->rows[f].recorded[item];
    }
    return total;
}

void recon_print_table(const ReconLedger* ledger, FILE* out) {
    if (!ledger || !out) return;
    fprintf(out, "frame");
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
        fprintf(out, "\t%s", kItemNames[i]);
    fprintf(out, "\n");
    for (uint32_t f = 0; f < ledger->count; f++) {
        const ReconRow* row = &ledger->rows[f];
        fprintf(out, "%u", row->frame);
        for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
            fprintf(out, "\t%lld", (long long)row->recorded[i]);
        fprintf(out, "\n");
    }
    fprintf(out, "total");
    for (unsigned i = 0; i < RECON_ITEM_COUNT; i++)
        fprintf(out, "\t%lld", (long long)recon_total(ledger, (ReconItem)i));
    fprintf(out, "\n");
}

int recon_verify(const ReconLedger* ledger, FILE* out) {
    if (!ledger || !out) return -1;
    int issues = 0;

    if (ledger->magic != RECON_FILE_MAGIC) {
        fprintf(out, "error: ledger magic mismatch\n");
        return 1;
    }
    if (ledger->version != RECON_VERSION || ledger->capacity != RECON_MAX_FRAMES) {
        fprintf(out, "error: ledger header mismatch\n");
        issues++;
    }
    if (ledger->count > ledger->capacity) {
        fprintf(out, "error: ledger count=%u exceeds capacity=%u\n",
                ledger->count, ledger->capacity);
        issues++;
    }
    if (!(ledger->flags & RECON_LEDGER_SEALED)) {
        fprintf(out, "error: ledger not sealed\n");
        issues++;
    } else if (ledger->sealed_frames != ledger->count) {
        fprintf(out, "error: ledger incomplete: seal expects %u frames, have %u\n",
                ledger->sealed_frames, ledger->count);
        issues++;
    }

    uint32_t limit = ledger->count > ledger->capacity ? ledger->capacity
                                                      : ledger->count;
    for (uint32_t f = 0; f < limit; f++) {
        issues += check_row_structure(&ledger->rows[f], f, out);
        issues += validate_cells(&ledger->rows[f], out);
    }
    return issues;
}

/* ---- persistence -------------------------------------------------------- */

int recon_save(const ReconLedger* ledger, const char* path) {
    if (!ledger || !path) return -1;
    if (!(ledger->flags & RECON_LEDGER_SEALED) ||
        ledger->sealed_frames != ledger->count || ledger->count > ledger->capacity)
        return -2;
    /* Refuse to persist a damaged table; a bad cell is a reconciliation
     * failure but a bad checksum means the table itself is corrupt. */
    for (uint32_t i = 0; i < ledger->count; i++) {
        const ReconRow* row = &ledger->rows[i];
        if (row->magic != RECON_ROW_MAGIC || row->frame != i ||
            row->checksum != row_checksum(row))
            return -5;
    }

    ReconFileHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = RECON_FILE_MAGIC;
    hdr.version = RECON_VERSION;
    hdr.row_size = (uint32_t)sizeof(ReconRow);
    hdr.capacity = ledger->capacity;
    hdr.count = ledger->count;
    hdr.sealed_frames = ledger->sealed_frames;
    hdr.checksum = header_checksum(&hdr);

    ReconFileTrailer trailer;
    trailer.magic = kTrailerMagic;
    trailer.ledger_checksum = ledger_checksum(ledger);

    FILE* f = fopen(path, "wb");
    if (!f) return -3;
    int rc = 0;
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) rc = -3;
    for (uint32_t i = 0; rc == 0 && i < ledger->count; i++)
        if (fwrite(&ledger->rows[i], sizeof(ReconRow), 1, f) != 1) rc = -3;
    if (rc == 0 && fwrite(&trailer, sizeof trailer, 1, f) != 1) rc = -3;
    if (fclose(f) != 0 && rc == 0) rc = -3;
    return rc;
}

int recon_load(ReconLedger* ledger, const char* path) {
    if (!ledger || !path) return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -3;

    ReconFileHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1) {
        fclose(f);
        return -4; /* truncated header */
    }
    if (hdr.magic != RECON_FILE_MAGIC || hdr.version != RECON_VERSION ||
        hdr.row_size != (uint32_t)sizeof(ReconRow) ||
        hdr.capacity != RECON_MAX_FRAMES || hdr.count > hdr.capacity) {
        fclose(f);
        return -4;
    }
    if (hdr.checksum != header_checksum(&hdr)) {
        fclose(f);
        return -5;
    }
    if (hdr.count != hdr.sealed_frames) {
        fclose(f);
        return -2; /* incomplete table */
    }

    ReconLedger loaded;
    memset(&loaded, 0, sizeof loaded);
    loaded.magic = RECON_FILE_MAGIC;
    loaded.version = hdr.version;
    loaded.capacity = hdr.capacity;
    loaded.count = hdr.count;
    loaded.sealed_frames = hdr.sealed_frames;
    loaded.flags = RECON_LEDGER_SEALED;

    for (uint32_t i = 0; i < hdr.count; i++) {
        if (fread(&loaded.rows[i], sizeof(ReconRow), 1, f) != 1) {
            fclose(f);
            return -2; /* short table: missing row */
        }
        if (loaded.rows[i].magic != RECON_ROW_MAGIC ||
            loaded.rows[i].frame != i ||
            loaded.rows[i].checksum != row_checksum(&loaded.rows[i])) {
            fclose(f);
            return -5; /* corrupt row */
        }
    }
    ReconFileTrailer trailer;
    if (fread(&trailer, sizeof trailer, 1, f) != 1) {
        fclose(f);
        return -2; /* missing trailer: incomplete */
    }
    if (trailer.magic != kTrailerMagic) {
        fclose(f);
        return -4;
    }
    if (trailer.ledger_checksum != ledger_checksum(&loaded)) {
        fclose(f);
        return -5;
    }
    /* Any trailing bytes mean this is not the table we wrote. */
    if (fgetc(f) != EOF) {
        fclose(f);
        return -4;
    }
    if (fclose(f) != 0) return -3;

    *ledger = loaded;
    recon_reset_counters();
    return 0;
}

/* ---- fault injection (tests only) -------------------------------------- */

int recon_inject_value_fault(ReconLedger* ledger, uint32_t frame,
                             ReconItem item, int64_t delta) {
    if (!ledger || frame >= ledger->count || (unsigned)item >= RECON_ITEM_COUNT)
        return -1;
    ReconRow* row = &ledger->rows[frame];
    row->recorded[item] += delta;
    row->rec_set |= (1u << item);
    row->checksum = row_checksum(row);
    return 0;
}

int recon_inject_missing(ReconLedger* ledger, uint32_t frame, ReconItem item) {
    if (!ledger || frame >= ledger->count || (unsigned)item >= RECON_ITEM_COUNT)
        return -1;
    ReconRow* row = &ledger->rows[frame];
    row->have &= ~(1u << item);
    row->checksum = row_checksum(row);
    return 0;
}
