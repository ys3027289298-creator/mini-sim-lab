/* det/evidence.c: shared evidence session; see evidence.h. */
#include "evidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void det_session_open(DetSession* s, const char* unit, const DetCli* cli,
                      size_t state_size) {
    memset(s, 0, sizeof(*s));
    s->unit = unit;
    s->state_size = state_size;
    s->shared = cli->shared;

    if (cli->load) {
        /* The file carries the context seed; det_context_load adopts it. */
        s->ctx = det_context_create(0);
        if (!s->ctx) det_cli_die(unit, "--load", "out of memory");
        char err[256];
        err[0] = '\0';
        if (det_context_load(s->ctx, cli->load, unit, err, sizeof(err)) != DET_OK) {
            if (err[0]) det_cli_fail(err);
            det_cli_die(unit, "--load", "rejected '%s'", cli->load);
        }
        void* st = det_unit_state(s->ctx, unit);
        if (!st) det_cli_die(unit, "--load", "instance has no '%s' state", unit);
        size_t got = det_unit_size(s->ctx, unit);
        if (got != state_size)
            det_cli_die(unit, "--load",
                        "state size is %zu bytes in '%s', this build expects %zu",
                        got, cli->load, state_size);
        if (cli->shared) {
            s->state = st;
        } else {
            s->state = calloc(1, state_size);
            if (!s->state) det_cli_die(unit, "--load", "out of memory");
            memcpy(s->state, st, state_size);
        }
        return;
    }

    s->ctx = det_context_create(cli->seed);
    if (!s->ctx) det_cli_die(unit, "--seed", "out of memory");
    if (cli->shared) {
        char err[256];
        err[0] = '\0';
        s->state = det_unit_open(s->ctx, unit, state_size, err, sizeof(err));
        if (!s->state) {
            if (err[0]) det_cli_fail(err);
            det_cli_die(unit, "--shared", "cannot open unit state");
        }
    } else {
        s->state = calloc(1, state_size);
        if (!s->state) det_cli_die(unit, "--no-shared", "out of memory");
    }
}

void det_session_close(DetSession* s) {
    if (!s) return;
    if (!s->shared) free(s->state);
    det_context_destroy(s->ctx);
    memset(s, 0, sizeof(*s));
}

void det_session_finish(DetSession* s, const DetCli* cli, uint64_t state_hash,
                        int expect_required) {
    if (cli->dump) {
        char err[256];
        err[0] = '\0';
        if (s->shared) {
            if (det_context_save(s->ctx, s->unit, cli->dump, err, sizeof(err)) != DET_OK) {
                if (err[0]) det_cli_fail(err);
                det_cli_die(s->unit, "--dump", "could not write '%s'", cli->dump);
            }
        } else {
            /* Snapshot the private block through a throwaway context so the
             * dumped bytes are identical to the --shared path. */
            DetContext* tmp = det_context_create(det_context_seed(s->ctx));
            if (!tmp) det_cli_die(s->unit, "--dump", "out of memory");
            void* dst = det_unit_open(tmp, s->unit, s->state_size, err, sizeof(err));
            if (!dst) {
                det_context_destroy(tmp);
                if (err[0]) det_cli_fail(err);
                det_cli_die(s->unit, "--dump", "cannot stage unit state");
            }
            memcpy(dst, s->state, s->state_size);
            int rc = det_context_save(tmp, s->unit, cli->dump, err, sizeof(err));
            det_context_destroy(tmp);
            if (rc != DET_OK) {
                if (err[0]) det_cli_fail(err);
                det_cli_die(s->unit, "--dump", "could not write '%s'", cli->dump);
            }
        }
    }
    if (expect_required && !cli->compare_given)
        det_cli_die(s->unit, "--expect-hash", "is required for this check");
    if (cli->compare_given && cli->compare != state_hash) {
        det_cli_die(s->unit, "--expect-hash",
                    "state mismatch: wanted %016llx, got %016llx",
                    (unsigned long long)cli->compare,
                    (unsigned long long)state_hash);
    }
}
