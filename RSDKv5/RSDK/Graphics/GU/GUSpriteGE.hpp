// Scaled sprites (DrawSpriteRotozoom without rotation) on the GE.
//
// The Special Stage's spheres and rings are FX_SCALE sprites drawn between the
// 3D models. On the GE they cost less and, more importantly, no longer split
// the 3D face batches into separate lists.
//
// Only the axis-aligned case is taken (deltaY == 0, deltaXLen == 0). The quad
// is clipped to exactly the pixels whose CPU sample lands inside the frame,
// and the texture coordinates are shifted half a step so the GE's pixel-centre
// samples hit the same texels. Rows are split by palette bank, as the floor is.

#ifndef GU_SPRITE_GE
#define GU_SPRITE_GE 1
#endif

static int32 gu_sprGeRouted  = 0; // scaled sprites taken by the GE (profile window)
static int32 gu_sprGeDecline = 0;

static inline int32 GU_Log2Pow2(int32 v)
{
    int32 n = 0;
    while ((1 << n) < v && n < 10)
        ++n;
    return ((1 << n) == v) ? n : -1;
}

// ceil(a / b) and floor(a / b) for b > 0 with a of either sign.
static inline int32 GU_FloorDiv(int32 a, int32 b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
static inline int32 GU_CeilDiv(int32 a, int32 b) { return -GU_FloorDiv(-a, b); }

// Pixel range [lo, hi) within [0, size) whose sample start + i*delta lies in
// [rectLo, rectHi). delta != 0.
static inline void GU_SampleRange(int32 start, int32 delta, int32 size, int32 rectLo, int32 rectHi, int32 *lo, int32 *hi)
{
    int32 a, b;
    if (delta > 0) {
        a = GU_CeilDiv(rectLo - start, delta); // start + i*delta >= rectLo
        b = GU_CeilDiv(rectHi - start, delta); // start + i*delta <  rectHi  <=>  i < b
    }
    else {
        const int32 d = -delta;                    // sample decreases with i
        a = GU_FloorDiv(start - rectHi, d) + 1;    // start - i*d <  rectHi  <=>  i > (start - rectHi)/d
        b = GU_FloorDiv(start - rectLo, d) + 1;    // start - i*d >= rectLo  <=>  i <= (start - rectLo)/d
    }
    if (a < 0)
        a = 0;
    if (b > size)
        b = size;
    *lo = a;
    *hi = b;
}

static bool GU_TryQueueScaledSpriteGE(int32 left, int32 top, int32 xSize, int32 ySize, int32 fullX, int32 fullY, int32 fullSprX, int32 fullSprY,
                                      int32 deltaX, int32 deltaY, int32 deltaXLen, int32 deltaYLen, int32 drawX, int32 drawY, int32 inkEffect,
                                      int32 sheetID)
{
#if !GU_SPRITE_GE
    (void)left; (void)top; (void)xSize; (void)ySize; (void)fullX; (void)fullY; (void)fullSprX; (void)fullSprY; (void)deltaX; (void)deltaY;
    (void)deltaXLen; (void)deltaYLen; (void)drawX; (void)drawY; (void)inkEffect; (void)sheetID;
    return false;
#else
    if (inkEffect != INK_NONE || deltaY != 0 || deltaXLen != 0 || deltaX == 0 || deltaYLen == 0)
        return false;
    if (!gu_roto_verts || !gu_tile_atlas_ok)
        return false;
    if (xSize <= 0 || ySize <= 0)
        return true; // nothing to draw, as on the CPU

    const GFXSurface *surface = &gfxSurface[sheetID];
    const int32 lw = GU_Log2Pow2(surface->width), lh = GU_Log2Pow2(surface->height);
    if (lw < 0 || lh < 0) {
        ++gu_sprGeDecline;
        return false; // TSIZE needs powers of two; the CPU keeps these
    }
    if (!GU_UploadSpriteTexture((uint16)sheetID)) {
        ++gu_sprGeDecline;
        return false; // no VRAM for the sheet
    }

    int32 xLo, xHi, yLo, yHi;
    GU_SampleRange(drawX, deltaX, xSize, fullSprX, fullX, &xLo, &xHi);
    GU_SampleRange(drawY, deltaYLen, ySize, fullSprY, fullY, &yLo, &yHi);
    if (xLo >= xHi || yLo >= yHi)
        return true; // every pixel's sample is outside the frame: the CPU draws nothing

    // One quad per run of rows sharing a palette bank.
    int32 pieces = 1;
    for (int32 y = yLo + 1; y < yHi; ++y)
        if (gfxLineBuffer[top + y] != gfxLineBuffer[top + y - 1])
            ++pieces;
    if (gu_draw_queue_count + pieces > GU_DRAW_QUEUE_MAX)
        GU_FlushDrawQueue();
    if (gu_draw_queue_count + pieces > GU_DRAW_QUEUE_MAX || gu_roto_vert_count + 2 * pieces > gu_roto_vert_max) {
        ++gu_sprGeDecline;
        return false;
    }

    const float k   = 1.0f / 65536.0f;
    const float du  = (float)deltaX * k;
    const float dv  = (float)deltaYLen * k;
    const float u0  = (float)drawX * k + (float)xLo * du - 0.5f * du; // pixel centres land on the CPU's texels
    const float u1  = u0 + (float)(xHi - xLo) * du;
    const s16 x0    = (s16)(left + xLo), x1 = (s16)(left + xHi);

    int32 y = yLo;
    while (y < yHi) {
        const int32 bank = gfxLineBuffer[top + y];
        int32 yEnd       = y + 1;
        while (yEnd < yHi && gfxLineBuffer[top + yEnd] == bank)
            ++yEnd;

        const float v0 = (float)drawY * k + (float)y * dv - 0.5f * dv;
        const float v1 = v0 + (float)(yEnd - y) * dv;

        // Consecutive sprites with the same sheet and bank share one batch.
        GUQueueEntry *e = NULL;
        if (gu_draw_queue_count > 0) {
            GUQueueEntry *last = &gu_draw_queue[gu_draw_queue_count - 1];
            if (last->type == GU_ENTRY_TILEBATCH && last->tileBatch.roto == 2 && last->tileBatch.sheet == sheetID
                && last->tileBatch.bank == bank && !last->tileBatch.submitted
                && last->tileBatch.firstVert + last->tileBatch.vertCount == gu_roto_vert_count)
                e = last;
        }
        if (!e) {
            e                          = &gu_draw_queue[gu_draw_queue_count++];
            e->type                    = GU_ENTRY_TILEBATCH;
            e->tileBatch.firstVert     = gu_roto_vert_count;
            e->tileBatch.vertCount     = 0;
            e->tileBatch.bank          = bank;
            e->tileBatch.roto          = 2;
            e->tileBatch.submitted     = 0;
            e->tileBatch.sheet         = (uint16)sheetID;
            e->tileBatch.screenSnapshot = *currentScreen;
        }

        // Written through the uncached alias, so no writeback is needed.
        GURotoVertex *v = (GURotoVertex *)((u32)&gu_roto_verts[gu_roto_vert_count] | 0x40000000);
        gu_roto_vert_count += 2;
        v[0].u = u0;  v[0].v = v0;  v[0].x = x0;  v[0].y = (s16)(top + y);    v[0].z = 0;  v[0].pad = 0;
        v[1].u = u1;  v[1].v = v1;  v[1].x = x1;  v[1].y = (s16)(top + yEnd); v[1].z = 0;  v[1].pad = 0;
        e->tileBatch.vertCount += 2;
        y = yEnd;
    }
    ++gu_sprGeRouted;
    return true;
#endif
}
