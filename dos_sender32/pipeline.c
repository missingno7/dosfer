#include "pipeline.h"
#include <stdlib.h>
#include <string.h>

/* The stage driver is intentionally small and deterministic.  Each public
 * step performs at most one queue transition; the DOS executable supplies
 * the QR-specific callbacks through Pipeline.context. */
int disk_refill_step(Pipeline *pl, FILE *file) {
    unsigned i;
    DiskBuf *d;
    if (!pl || !file) return 0;
    for (i = 0; i < PL_DISK_SLOTS; ++i) {
        d = &pl->disk[(pl->disk_head + i) % PL_DISK_SLOTS];
        if (d->state != DS_EMPTY) continue;
        d->state = DS_FILLING;
        d->pos = 0;
        d->len = (unsigned)fread(d->buf, 1, PL_DISK_SLOT_BYTES, file);
        if (!d->len) { d->state = DS_EMPTY; return 0; }
        d->state = DS_READY;
        pl->disk_head = (uint8_t)((d - pl->disk + 1u) % PL_DISK_SLOTS);
        ++pl->disk_ready;
        return 1;
    }
    return 0;
}

int input_builder_step(Pipeline *pl, RecordStream *stream) {
    if (!pl || !pl->input_builder) return 0;
    return pl->input_builder(pl, stream);
}

int v40_worker_step(Pipeline *pl, WorkMemory *worker, Vga32 *vga, int verify) {
    if (!pl || !pl->qr_worker) return 0;
    return pl->qr_worker(pl, worker, vga, verify);
}

int prepared_upload_step(Pipeline *pl, WorkMemory *worker, Vga32 *vga) {
    if (!pl || !pl->prepared_upload) return 0;
    return pl->prepared_upload(pl, worker, vga);
}

int playback_step(Pipeline *pl, Vga32 *vga) {
    if (!pl || !pl->playback) return 0;
    return pl->playback(pl, vga);
}

int pipeline_init(Pipeline *pl, WorkMemory *worker, Vga32 *vga) {
    unsigned i;
    (void)worker; (void)vga;
    if (!pl) return 0;
    memset(pl, 0, sizeof(*pl));
    for (i = 0; i < PL_DISK_SLOTS; ++i) {
        pl->disk[i].buf = (uint8_t *)malloc(PL_DISK_SLOT_BYTES);
        if (!pl->disk[i].buf) { pipeline_shutdown(pl); return 0; }
        pl->disk[i].state = DS_EMPTY;
    }
    for (i = 0; i < PL_INPUT_SLOTS; ++i) pl->input[i].state = IS_FREE;
    for (i = 0; i < PL_PREP_SLOTS; ++i) pl->prep[i].state = PS_FREE;
    for (i = 0; i < PL_VGA_SLOTS; ++i) { pl->vga[i].slot = (uint8_t)i; pl->vga[i].state = VS_FREE; }
    pl->start_ticks = 0;
    return 1;
}

void pipeline_shutdown(Pipeline *pl) {
    unsigned i;
    if (!pl) return;
    for (i = 0; i < PL_DISK_SLOTS; ++i) {
        free(pl->disk[i].buf);
        pl->disk[i].buf = 0;
        pl->disk[i].state = DS_EMPTY;
    }
}
