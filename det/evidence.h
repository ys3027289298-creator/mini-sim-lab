/* det/evidence.h: shared plumbing for the module evidence drivers.
 *
 * A session owns the module's canonical state block.  With --shared the block
 * is a unit inside the shared context; with --no-shared it is a private
 * calloc'd block.  --dump and --load always move canonical bytes, so both
 * modes agree byte for byte. */
#ifndef DET_EVIDENCE_H
#define DET_EVIDENCE_H

#include "cli.h"
#include "detcontext.h"

typedef struct {
    const char* unit;
    DetContext* ctx;      /* present in both modes (private one for snapshots) */
    void* state;
    size_t state_size;
    int shared;
} DetSession;

/* Opens the session and zeroes the state block.  When --load is given the
 * block is restored from the snapshot; a file lacking this unit or carrying
 * the wrong state size is rejected with a module-located message. */
void det_session_open(DetSession* s, const char* unit, const DetCli* cli,
                      size_t state_size);
void det_session_close(DetSession* s);

/* Writes the session state, then checks --expect-hash against the digest the
 * module computed over the same canonical state image. */
void det_session_finish(DetSession* s, const DetCli* cli, uint64_t state_hash,
                        int expect_required);

#endif /* DET_EVIDENCE_H */
