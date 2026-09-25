// Depth-carrying DrawFace polygons (the Special Stage's shadows) go into the GE
// face batch instead of through the CPU rasterizer. Draw3DScene sets
// gu_faceDepthValid around its DrawFace calls; any other DrawFace (HUD, menus)
// stays on the CPU.
//
// INK_BLEND uses fixed 0x80 blend factors, which can differ from the CPU's
// (a + b) >> 1 by one LSB per channel.

#ifndef GU_FACE_GE
#define GU_FACE_GE 1
#endif

#ifndef GE_CMD_BLEND
#define GE_CMD_BLEND 0xdf // blend equation | src func << 4 | dst func << 8
#define GE_CMD_SFIX  0xe0
#define GE_CMD_DFIX  0xe1
#endif

extern "C" int32 gu_faceBatchAvailable; // defined with C linkage for S3DFast.hpp
int32 gu_faceDepthValid = 0;          // set by Draw3DScene around DrawFace; extern in Scene3D.cpp
static int32 gu_faceGeRouted   = 0;   // DrawFace polygons taken by the batch (profile window)

static bool GU_TryQueueFaceGE(const Vector2 *vertices, int32 vertCount, int32 b, int32 g, int32 r, int32 inkEffect)
{
#if !GU_FACE_GE
    (void)vertices; (void)vertCount; (void)b; (void)g; (void)r; (void)inkEffect;
    return false;
#else
    if (!gu_face_verts || !gu_faceBatchAvailable)
        return false;
    if (vertCount != 3 && vertCount != 4)
        return false;

    int32 blend;
    if (inkEffect == INK_NONE)
        blend = 0;
    else if (inkEffect == INK_BLEND)
        blend = 1;
    else
        return false;

    // Through-mode coordinates are 16-bit: a far-off-screen face would wrap.
    for (int32 i = 0; i < vertCount; ++i) {
        const int32 px = vertices[i].x >> 16, py = vertices[i].y >> 16;
        if (px < -1024 || px > 2047 || py < -1024 || py > 2047)
            return false;
    }

    const int32 needed = (vertCount == 3) ? 3 : 6;

    GU_DrainQueueIfFull();
    if (gu_face_vert_count + needed > gu_face_vert_max)
        return false;

    // Blending is per-batch state, so only extend a batch with the same mode.
    GUQueueEntry *bt = NULL;
    if (gu_draw_queue_count > 0 && gu_draw_queue[gu_draw_queue_count - 1].type == GU_ENTRY_FACEBATCH
        && gu_draw_queue[gu_draw_queue_count - 1].faceBatch.blend == blend
        && !gu_draw_queue[gu_draw_queue_count - 1].faceBatch.submitted)
        bt = &gu_draw_queue[gu_draw_queue_count - 1];
    else {
        bt                          = &gu_draw_queue[gu_draw_queue_count++];
        bt->type                    = GU_ENTRY_FACEBATCH;
        bt->faceBatch.screenSnapshot = *currentScreen;
        bt->faceBatch.firstVert     = gu_face_vert_count;
        bt->faceBatch.vertCount     = 0;
        bt->faceBatch.blend         = (uint8)blend;
        bt->faceBatch.submitted     = 0;
    }

    int32 dz = gu_faceDepth >> GU_DEPTH_SHIFT;
    if (dz < 0)
        dz = 0;
    if (dz > GU_DEPTH_FAR)
        dz = GU_DEPTH_FAR;

    // 0xAABBGGRR -- red in the low byte, as the framebuffer has it.
    const u32 color = 0xFF000000u | ((u32)(b & 0xFF) << 16) | ((u32)(g & 0xFF) << 8) | (u32)(r & 0xFF);

    static const int32 quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int32 i = 0; i < needed; ++i) {
        const int32 s   = (vertCount == 3) ? i : quadIdx[i];
        GUFaceVertex *v = &gu_face_verts[gu_face_vert_count++];
        v->color        = color;
        v->x            = (s16)(vertices[s].x >> 16);
        v->y            = (s16)(vertices[s].y >> 16);
        v->z            = (s16)dz;
        v->pad          = 0;
    }
    bt->faceBatch.vertCount += needed;
    ++gu_faceGeRouted;
    return true;
#endif
}
