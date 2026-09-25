// Rotozoom tile layers on the GE, in through mode.
//
// Each scanline of a rotozoom layer walks a straight line through tile space,
// so it can be drawn as textured line segments, one per tile crossing, out of
// the same atlas and CLUT the HSCROLL layers use. This replaces a per-pixel
// CPU loop that was the largest single cost in the Special Stage.
//
// Texture coordinates are floats: rows start at fractional texels, and
// rounding them would make the tile seams wobble near the horizon. The result
// is not bit-exact with the CPU path, since the GE interpolates in its own
// fixed point.

#ifndef GU_ROTO_GPU
#define GU_ROTO_GPU 1
#endif

struct GURotoVertex {
    float u, v;
    s16 x, y, z;
    s16 pad;
};
static_assert(sizeof(GURotoVertex) == 16, "GE stride: float uv then s16 xyz, padded to the float alignment");

// Vertex pool, in VRAM. Written through the cached alias and written back per
// batch. A layer that does not fit is rolled back and drawn on the CPU whole.
#define GU_ROTO_VERT_MAX 16384 // 8192 segments; the hardware peak seen is 5,362
static GURotoVertex *gu_roto_verts = NULL;
static int32 gu_roto_vert_count    = 0;
static int32 gu_roto_vert_max      = 0;

static int32 gu_rotoGpuSegs    = 0; // segments emitted (per profile window)
static int32 gu_rotoGpuRows    = 0;
static int32 gu_rotoGpuLayers  = 0;
static int32 gu_rotoGpuDecline = 0; // layers sent back to the CPU (pool or queue full)
static SceUInt64 gu_rotoGpuUsec = 0; // CPU time building the segments

// floor(2^32 / |d|), or 0 for d == 0 (which the caller never divides by).
static inline uint32 GU_RotoRecip(int32 d)
{
    if (d < 0)
        d = -d;
    if (d == 0)
        return 0;
    return (uint32)(0xFFFFFFFFu / (uint32)d);
}

// ceil(room / d) for room >= 1, d >= 1, with rcp = GU_RotoRecip(d). The
// estimate from the rounded-down reciprocal is at most one short; the two
// compares make it exact.
static inline int32 GU_RotoCeilDiv(int32 room, int32 d, uint32 rcp)
{
    uint32 q = (uint32)(((uint64)(uint32)room * rcp) >> 32);
    if ((q + 1) * (uint32)d <= (uint32)room)
        ++q;                              // rcp was rounded down
    if (q * (uint32)d < (uint32)room)
        ++q;                              // floor -> ceil
    return (int32)q;
}

static bool GU_TryQueueRotozoomGPU(TileLayer *layer)
{
#if !GU_ROTO_GPU
    (void)layer;
    return false;
#else
    if (layer->type != LAYER_ROTOZOOM)
        return false;
    if (!gu_roto_verts)
        return false;
    if (!layer->xsize || !layer->ysize)
        return false;

    // Same once-per-frame atlas refresh as the HSCROLL path.
    if (gu_atlas_frame != gu_frameCounter) {
        gu_atlas_frame = gu_frameCounter;
        GU_BuildTileAtlas();
    }
    if (!gu_tile_atlas_ok)
        return false;

    const int32 clipX1 = currentScreen->clipBound_X1, clipX2 = currentScreen->clipBound_X2;
    const int32 clipY1 = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    if (clipX1 >= clipX2 || clipY1 >= clipY2)
        return false;

    const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    // One batch per run of rows sharing a palette bank, since the CLUT is batch
    // state. Reserve the queue entries up front: a flush mid-layer would reset
    // the pool under the batches already queued.
    int32 bankRuns = 1;
    for (int32 cy = clipY1 + 1; cy < clipY2; ++cy)
        if (gfxLineBuffer[cy] != gfxLineBuffer[cy - 1])
            ++bankRuns;
    if (gu_draw_queue_count + bankRuns > GU_DRAW_QUEUE_MAX)
        GU_FlushDrawQueue();
    if (gu_draw_queue_count + bankRuns > GU_DRAW_QUEUE_MAX) {
        ++gu_rotoGpuDecline;
        return false;
    }

    const uint16 *layout   = layer->layout;
    const int32 widthShift = layer->widthShift;
    const int32 tileMaskX  = ((TILE_SIZE << widthShift) - 1) >> 4;
    const int32 tileMaskY  = ((TILE_SIZE << layer->heightShift) - 1) >> 4;
    const int32 lineSize   = clipX2 - clipX1;
    const float texelScale = 1.0f / 65536.0f;

    const int32 queueStart = gu_draw_queue_count;
    const int32 poolStart  = gu_roto_vert_count;
    int32 batchFirst       = gu_roto_vert_count;
    int32 batchY1          = clipY1;
    int32 curBank          = gfxLineBuffer[clipY1];
    int32 segs             = 0;
    bool full              = false;

    for (int32 cy = clipY1; cy < clipY2 && !full; ++cy) {
        const int32 bank = gfxLineBuffer[cy];
        if (bank != curBank) {
            const int32 used = gu_roto_vert_count - batchFirst;
            if (used) {
                sceKernelDcacheWritebackRange(&gu_roto_verts[batchFirst], sizeof(GURotoVertex) * used);
                GUQueueEntry *e        = &gu_draw_queue[gu_draw_queue_count++];
                e->type                = GU_ENTRY_TILEBATCH;
                e->tileBatch.firstVert = batchFirst;
                e->tileBatch.vertCount = used;
                e->tileBatch.bank      = curBank;
                e->tileBatch.roto      = 1;
                e->tileBatch.submitted = 0;
                e->tileBatch.screenSnapshot              = *currentScreen;
                e->tileBatch.screenSnapshot.clipBound_Y1 = batchY1;
                e->tileBatch.screenSnapshot.clipBound_Y2 = cy;
            }
            batchFirst = gu_roto_vert_count;
            batchY1    = cy;
            curBank    = bank;
        }

        const ScanlineInfo *sl = &scanlines[cy];
        int32 posX             = sl->position.x;
        int32 posY             = sl->position.y;
        const int32 dX         = sl->deform.x;
        const int32 dY         = sl->deform.y;
        const float du         = (float)dX * texelScale;
        const float dv         = (float)dY * texelScale;

        // A segment runs until the cursor leaves its tile: with `room` the
        // distance to the boundary it is heading for, that is ceil(room / |d|)
        // pixels on each axis.
        const uint32 rcpX = GU_RotoRecip(dX);
        const uint32 rcpY = GU_RotoRecip(dY);
        const int32 adX   = dX < 0 ? -dX : dX;
        const int32 adY   = dY < 0 ? -dY : dY;

        // Fast path for dY == 0 (every row of the Special Stage floor): the row
        // stays in one tile row, so the layout row, v and the Y divide are
        // per-row constants, and segment lengths follow a recurrence instead of
        // a divide. A segment of n = ceil(r / d) pixels overshoots its tile by
        // o = n*d - r in [0, d), and the next tile's room is 2^20 - o. With
        // 2^20 = Q*d + R, the next segment is Q + (R > o) pixels long.
        if (dY == 0 && dX != 0) {
            const int32 d    = adX;
            const int32 step = dX > 0 ? 1 : -1;
            const int32 Q    = (1 << 20) / d;
            const int32 R    = (1 << 20) - Q * d;

            const int32 rowBase = (tileMaskY & (posY >> 20)) << widthShift;
            const float vRow    = (float)(posY & 0xFFFFF) * texelScale; // dv == 0
            const float halfDu  = 0.5f * du;

            int32 tx = posX >> 20;
            int32 r  = dX > 0 ? (((tx + 1) << 20) - posX) : (posX - (tx << 20) + 1);
            int32 n  = GU_RotoCeilDiv(r, d, rcpX);
            int32 o  = n * d - r;

            int32 xx = 0;
            while (xx < lineSize) {
                int32 emit = lineSize - xx;
                if (n < emit)
                    emit = n;

                const uint16 entry = layout[rowBase + (tileMaskX & tx)];
                if (entry != 0xFFFF) {
                    if (gu_roto_vert_count + 2 > gu_roto_vert_max) {
                        full = true;
                        break;
                    }
                    const int32 tile = entry & 0x3FF;
                    const int32 flip = (entry >> 10) & 3;
                    const float au   = (float)((tile % GU_ATLAS_TILES_ROW) * TILE_SIZE);
                    const float av   = (float)((tile / GU_ATLAS_TILES_ROW) * TILE_SIZE);

                    // Offset into the tile: 2^20 - r going right, r - 1 going left.
                    float u0 = (float)(dX > 0 ? ((1 << 20) - r) : (r - 1)) * texelScale - halfDu;
                    float u1 = u0 + (float)emit * du;
                    float v0 = vRow;
                    float v1 = vRow;
                    if (u0 < 0.0f) u0 = 0.0f;
                    if (u0 > 15.99f) u0 = 15.99f;
                    if (u1 < 0.0f) u1 = 0.0f;
                    if (u1 > 15.99f) u1 = 15.99f;
                    if (v0 < 0.0f) v0 = 0.0f;
                    if (v0 > 15.99f) v0 = 15.99f;
                    v1 = v0;
                    if (flip & 1) {
                        u0 = 16.0f - u0;
                        u1 = 16.0f - u1;
                    }
                    if (flip & 2) {
                        v0 = 16.0f - v0;
                        v1 = v0;
                    }

                    GURotoVertex *v = &gu_roto_verts[gu_roto_vert_count];
                    gu_roto_vert_count += 2;
                    v[0].u = au + u0;
                    v[0].v = av + v0;
                    v[0].x = (s16)(clipX1 + xx);
                    v[0].y = (s16)cy;
                    v[0].z = 0;
                    v[0].pad = 0;
                    v[1].u = au + u1;
                    v[1].v = av + v1;
                    v[1].x = (s16)(clipX1 + xx + emit);
                    v[1].y = (s16)cy;
                    v[1].z = 0;
                    v[1].pad = 0;
                    ++segs;
                }

                xx += emit;
                tx += step;
                r = (1 << 20) - o;
                if (R > o) {
                    n = Q + 1;
                    o = o + d - R;
                }
                else {
                    n = Q;
                    o = o - R;
                }
            }
            continue;
        }

        int32 x = 0;
        while (x < lineSize) {
            const int32 tx     = posX >> 20;
            const int32 ty     = posY >> 20;
            const int32 startX = posX;
            const int32 startY = posY;

            int32 n = lineSize - x;
            if (dX) {
                const int32 room = dX > 0 ? (((tx + 1) << 20) - posX) : (posX - (tx << 20) + 1);
                const int32 c    = GU_RotoCeilDiv(room, adX, rcpX);
                if (c < n)
                    n = c;
            }
            if (dY) {
                const int32 room = dY > 0 ? (((ty + 1) << 20) - posY) : (posY - (ty << 20) + 1);
                const int32 c    = GU_RotoCeilDiv(room, adY, rcpY);
                if (c < n)
                    n = c;
            }
            posX += n * dX;
            posY += n * dY;

            const uint16 entry = layout[(tileMaskX & tx) + ((tileMaskY & ty) << widthShift)];
            if (entry != 0xFFFF) {
                if (gu_roto_vert_count + 2 > gu_roto_vert_max) {
                    full = true;
                    break;
                }

                // Bits 10-11 are the flip flags; here a flip runs the texture
                // coordinate backwards across the tile.
                const int32 tile = entry & 0x3FF;
                const int32 flip = (entry >> 10) & 3;
                const float au   = (float)((tile % GU_ATLAS_TILES_ROW) * TILE_SIZE);
                const float av   = (float)((tile / GU_ATLAS_TILES_ROW) * TILE_SIZE);

                // Position inside the tile in texels. The low 20 bits are the
                // offset from the tile origin for negative positions too.
                float u0 = (float)(startX & 0xFFFFF) * texelScale;
                float v0 = (float)(startY & 0xFFFFF) * texelScale;
                float u1 = u0 + (float)n * du;
                float v1 = v0 + (float)n * dv;

                // The PSP samples lines at pixel centres, so shift the endpoints
                // back half a step to hit the texels the CPU path samples, and
                // keep them inside the tile.
                u0 -= 0.5f * du;
                u1 -= 0.5f * du;
                v0 -= 0.5f * dv;
                v1 -= 0.5f * dv;
                if (u0 < 0.0f) u0 = 0.0f;
                if (u0 > 15.99f) u0 = 15.99f;
                if (u1 < 0.0f) u1 = 0.0f;
                if (u1 > 15.99f) u1 = 15.99f;
                if (v0 < 0.0f) v0 = 0.0f;
                if (v0 > 15.99f) v0 = 15.99f;
                if (v1 < 0.0f) v1 = 0.0f;
                if (v1 > 15.99f) v1 = 15.99f;
                if (flip & 1) {
                    u0 = 16.0f - u0;
                    u1 = 16.0f - u1;
                }
                if (flip & 2) {
                    v0 = 16.0f - v0;
                    v1 = 16.0f - v1;
                }

                GURotoVertex *v = &gu_roto_verts[gu_roto_vert_count];
                gu_roto_vert_count += 2;
                v[0].u = au + u0;
                v[0].v = av + v0;
                v[0].x = (s16)(clipX1 + x);
                v[0].y = (s16)cy;
                v[0].z = 0;
                v[0].pad = 0;
                v[1].u = au + u1;
                v[1].v = av + v1;
                v[1].x = (s16)(clipX1 + x + n);
                v[1].y = (s16)cy;
                v[1].z = 0;
                v[1].pad = 0;
                ++segs;
            }
            x += n;
        }
    }

    if (full) {
        // Whole layer or nothing: a partial layer would leave a visible hole.
        gu_roto_vert_count  = poolStart;
        gu_draw_queue_count = queueStart;
        ++gu_rotoGpuDecline;
        return false;
    }

    {
        const int32 used = gu_roto_vert_count - batchFirst;
        if (used) {
            sceKernelDcacheWritebackRange(&gu_roto_verts[batchFirst], sizeof(GURotoVertex) * used);
            GUQueueEntry *e        = &gu_draw_queue[gu_draw_queue_count++];
            e->type                = GU_ENTRY_TILEBATCH;
            e->tileBatch.firstVert = batchFirst;
            e->tileBatch.vertCount = used;
            e->tileBatch.bank      = curBank;
            e->tileBatch.roto      = 1;
            e->tileBatch.submitted = 0;
            e->tileBatch.screenSnapshot              = *currentScreen;
            e->tileBatch.screenSnapshot.clipBound_Y1 = batchY1;
            e->tileBatch.screenSnapshot.clipBound_Y2 = clipY2;
        }
    }

    if (gu_profilingEnabled) {
        gu_rotoGpuUsec += sceKernelGetSystemTimeWide() - t0;
        gu_rotoGpuSegs += segs;
        gu_rotoGpuRows += clipY2 - clipY1;
        ++gu_rotoGpuLayers;
    }
    if (gu_layerLog)
        fprintf(gu_layerLog, "  GPU  type=%d draw=%d size=%dx%d rotozoom segs=%d bankRuns=%d verts=%d\n", (int)layer->type,
                (int)layer->drawGroup[0], (int)layer->xsize, (int)layer->ysize, (int)segs, (int)bankRuns,
                (int)(gu_roto_vert_count - poolStart));
    return true;
#endif
}
