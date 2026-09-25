// Asynchronous GE lists.
//
// GE lists are enqueued without waiting. The CPU only waits where it is about
// to touch what the GE draws into: before a CPU-rasterized queue entry, before
// the vertex pools are reset, and before the present DMA. Lists complete in
// order, so syncing on the last one retires them all.
//
// Consecutive GE batches share one command list, built in a ring of buffers so
// the words survive until the GE has read them. The queue's leading GE-only run
// is also submitted early (GU_GeTryEager), so the GE draws the sky, floor and
// 3D while the CPU is still running entity Draw() code.

#ifndef GU_GE_ASYNC
#define GU_GE_ASYNC 1
#endif

// The lists draw straight into the framebuffer, so it has to live in VRAM.
#if !GU_FB_IN_VRAM
#error "GUGeAsync.hpp needs GU_FB_IN_VRAM"
#endif

#define GU_GE_RING 8

static u32 *gu_geRing = NULL; // GU_GE_RING * GE_TRI_CMD_WORDS words in VRAM, cached alias
static int gu_geRingQid[GU_GE_RING];
static int32 gu_geRingNext = 0;
static int32 gu_gePending  = 0;  // lists enqueued and not yet retired
static int gu_geLastQid    = -1;

static SceUInt64 gu_geWaitUsec = 0; // CPU time spent in the deferred waits
static int32 gu_geWaits    = 0;
static int32 gu_geLists    = 0;
static int32 gu_geEager    = 0;     // early submissions (GU_GeTryEager)
static SceUInt64 gu_geEagerUsec = 0; // CPU time inside GU_GeTryEager

// Base of a command buffer to build the next list in.
static u32 *GU_GeAcquireCmd()
{
#if GU_GE_ASYNC
    if (gu_geRing) {
        const int32 slot = gu_geRingNext;
        gu_geRingNext    = (slot + 1) % GU_GE_RING;
        if (gu_geRingQid[slot] >= 0) {
            const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;
            sceGeListSync(gu_geRingQid[slot], 0);
            if (gu_profilingEnabled)
                gu_geWaitUsec += sceKernelGetSystemTimeWide() - t0;
            gu_geRingQid[slot] = -1;
        }
        return gu_geRing + slot * GE_TRI_CMD_WORDS;
    }
#endif
    return ge_tri_cmd;
}

// Submits a finished list built at `cmd`. Without the ring this is the old
// enqueue-and-wait.
static void GU_GeSubmit(u32 *cmd)
{
    const int qid = sceGeListEnQueue(cmd, NULL, gecbid, NULL);
    ++gu_geLists;
#if GU_GE_ASYNC
    if (gu_geRing && cmd != ge_tri_cmd) {
        const int32 slot   = (int32)((cmd - gu_geRing) / GE_TRI_CMD_WORDS);
        gu_geRingQid[slot] = qid;
        if (qid >= 0) {
            gu_geLastQid = qid;
            ++gu_gePending;
        }
        return;
    }
#endif
    if (qid >= 0)
        sceGeListSync(qid, 0);
    GU_FB_GE_DONE();
}

// The open list: batches append to it, and it is closed (terminated, written
// back, enqueued) only before a CPU draw, at the end of a flush, or when full.
bool gu_fbDirtyForGe = false; // a CPU draw touched the framebuffer since the last list
static u32 *gu_listBase     = NULL; // open list, or NULL
static u32 *gu_listSavedPtr = NULL; // ge_cmd_ptr to restore on close
static int32 gu_listBatches = 0;    // batches in the open list

static void GU_ListClose();

// Vertex/index ranges the open list reads. Batches note them and the list
// close writes each back once, instead of one syscall per batch. A range
// within 1 MB of an existing one grows it; overflow folds into the first.
#define GU_WB_SLOTS 4
static u32 gu_wbLo[GU_WB_SLOTS], gu_wbHi[GU_WB_SLOTS];
static int32 gu_wbCount = 0;

static inline void GU_WbNote(const void *ptr, u32 bytes)
{
    const u32 lo = (u32)ptr, hi = lo + bytes;
    if (bytes == 0)
        return;
    for (int32 i = 0; i < gu_wbCount; ++i) {
        if (lo + (1 << 20) > gu_wbLo[i] && gu_wbLo[i] + (1 << 20) > lo) {
            if (lo < gu_wbLo[i]) gu_wbLo[i] = lo;
            if (hi > gu_wbHi[i]) gu_wbHi[i] = hi;
            return;
        }
    }
    if (gu_wbCount < GU_WB_SLOTS) {
        gu_wbLo[gu_wbCount] = lo;
        gu_wbHi[gu_wbCount] = hi;
        ++gu_wbCount;
        return;
    }
    if (lo < gu_wbLo[0]) gu_wbLo[0] = lo;
    if (hi > gu_wbHi[0]) gu_wbHi[0] = hi;
}

static inline void GU_WbFlush()
{
    for (int32 i = 0; i < gu_wbCount; ++i)
        sceKernelDcacheWritebackRange((void *)gu_wbLo[i], gu_wbHi[i] - gu_wbLo[i]);
    gu_wbCount = 0;
}

// Make sure a list is open with at least `needWords` free.
static void GU_ListOpen(int32 needWords)
{
    if (gu_listBase && (int32)(ge_cmd_ptr - gu_listBase) + needWords > GE_TRI_CMD_WORDS - 4)
        GU_ListClose();
    if (!gu_listBase) {
        // Only write the framebuffer back if a CPU draw dirtied it. A
        // whole-cache writeback-invalidate here evicted the entity code's
        // working set on every list.
        if (gu_fbDirtyForGe) {
            sceKernelDcacheWritebackRange(screen_pixels, PRESENT_BUFFER_BYTES);
            gu_fbDirtyForGe = false;
        }
        gu_listBase     = GU_GeAcquireCmd();
        gu_listSavedPtr = ge_cmd_ptr;
        ge_cmd_ptr      = gu_listBase;
        gu_listBatches  = 0;
    }
}

static void GU_ListClose()
{
    if (!gu_listBase)
        return;
    if (gu_listBatches == 0) {
        ge_cmd_ptr  = gu_listSavedPtr;
        gu_listBase = NULL;
        gu_wbCount  = 0;
        return;
    }
    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);
    const int32 words = (int32)(ge_cmd_ptr - gu_listBase);
    ge_cmd_ptr        = gu_listSavedPtr;
    GU_WbFlush();
    sceKernelDcacheWritebackRange(gu_listBase, (size_t)words * sizeof(u32));
    GU_GeSubmit(gu_listBase);
    gu_listBase = NULL;
}

// Retires every list in flight. Required before the CPU reads or writes the
// framebuffer, and before the vertex pools are reset.
static void GU_GeWaitAll()
{
    GU_ListClose(); // an open list has not run yet
    if (!gu_gePending)
        return;
    const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;
    sceGeListSync(gu_geLastQid, 0); // in-order completion: this retires all of them
    for (int32 i = 0; i < GU_GE_RING; ++i)
        gu_geRingQid[i] = -1;
    gu_gePending = 0;
    gu_geLastQid = -1;
    GU_FB_GE_DONE();
    if (gu_profilingEnabled) {
        gu_geWaitUsec += sceKernelGetSystemTimeWide() - t0;
        ++gu_geWaits;
    }
}

// Submits the queue's leading GE-only run (tile, face and indexed batches) now,
// while nothing CPU-drawn sits ahead of it. A face or sprite batch that is the
// last entry stays open, since the next draw may extend it.
static void GU_GeTryEager()
{
#if GU_GE_ASYNC
    if (!gu_geRing)
        return;
    const SceUInt64 te0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;
    int32 n = gu_draw_queue_count;
    for (int32 i = 0; i < n; ++i) {
        const uint8 t = gu_draw_queue[i].type;
        if (t != GU_ENTRY_TILEBATCH && t != GU_ENTRY_FACEBATCH && t != GU_ENTRY_IDXBATCH) {
            n = i;
            break;
        }
    }
    if (n == gu_draw_queue_count && n > 0
        && (gu_draw_queue[n - 1].type == GU_ENTRY_FACEBATCH
            || (gu_draw_queue[n - 1].type == GU_ENTRY_TILEBATCH && gu_draw_queue[n - 1].tileBatch.roto == 2)))
        --n;
    bool any = false;
    int32 i  = 0;
    while (i < n) {
        if (gu_draw_queue[i].type == GU_ENTRY_TILEBATCH) {
            int32 last = i;
            bool pending = false;
            while (last + 1 < n && gu_draw_queue[last + 1].type == GU_ENTRY_TILEBATCH && (last - i + 1) < GU_TILE_RUN_MAX)
                ++last;
            for (int32 k = i; k <= last; ++k)
                if (!gu_draw_queue[k].tileBatch.submitted)
                    pending = true;
            if (pending)
                GU_DrawTileBatchRun(i, last - i + 1); // skips entries already submitted
            for (int32 k = i; k <= last; ++k) {
                if (!gu_draw_queue[k].tileBatch.submitted)
                    any = true;
                gu_draw_queue[k].tileBatch.submitted = 1;
            }
            i = last + 1;
        }
        else if (gu_draw_queue[i].type == GU_ENTRY_IDXBATCH) {
            GUIdxBatchEntry *ib = &gu_draw_queue[i].idxBatch;
            if (!ib->submitted) {
                ScreenInfo snapshotScreen = ib->screenSnapshot;
                ScreenInfo *saved         = currentScreen;
                currentScreen             = &snapshotScreen;
                GU_DrawFaceBatch((const GUFaceVertex *)ib->verts, ib->vertCount, 0, ib->indices, ib->indexCount);
                currentScreen = saved;
                ib->submitted = 1;
                any           = true;
            }
            ++i;
        }
        else {
            GUFaceBatchEntry *fb = &gu_draw_queue[i].faceBatch;
            if (!fb->submitted) {
                ScreenInfo snapshotScreen = fb->screenSnapshot;
                ScreenInfo *saved         = currentScreen;
                currentScreen             = &snapshotScreen;
                GU_DrawFaceBatch(&gu_face_verts[fb->firstVert], fb->vertCount, fb->blend, NULL, 0);
                currentScreen = saved;
                fb->submitted = 1;
                any           = true;
            }
            ++i;
        }
    }
    GU_ListClose();
    if (any)
        ++gu_geEager;
    if (gu_profilingEnabled)
        gu_geEagerUsec += sceKernelGetSystemTimeWide() - te0;
#endif
}

// Rebinds the texture to a sprite sheet after GU_EmitTileTextureState has set
// the CLUT and sampling state. Lives here because GE_CMD is defined after
// GUSpriteGE.hpp is included.
static void GU_EmitSheetTexture(int32 sheetID)
{
    const GUSpriteTex *tex = &gfxSurfaceGU[sheetID];
    const u32 addr         = (u32)tex->vramPixels;
    GE_CMD(TBP0, addr & 0x00FFFFFF);
    GE_CMD(TBW0, ((addr & 0xFF000000) >> 8) | (u32)tex->width);
    GE_CMD(TSIZE0, (GU_Log2Pow2(tex->height) << 8) | GU_Log2Pow2(tex->width));
    GE_CMD(TFLUSH, 0);
}
