#define MANIA_WIDTH (424)//424
#define MANIA_HEIGHT (240)//240

#include <pspkernel.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psputils.h>
#include <malloc.h>
#include <pspdmac.h>
#include <cstdio>
using namespace RSDK;

// Row stride for the rasterizer surface, and the value the present quad's
// TBW0 uses.
//
// 64-pixel aligned, NOT the engine's usual 16. sceGuDrawBuffer encodes the
// frame-buffer width in 64-pixel multiples, so a 16-aligned stride (424 ->
// 432) is not a legal GE render target at all -- the GE simply cannot draw
// into this surface. Rounding to 64 instead (424 -> 448) makes it legal, at
// a cost of 16 extra pixels of padding per row (~7.7KB total).
//
// SetScreenSize() applies the same 64-alignment for the GU render device, so
// screens[0].pitch matches this; the two must agree or the rasterizers will
// stride differently from the allocation and the present DMA.
//
// Only the stride changes -- the engine still draws 424x240, and the present
// quad's texture coordinates come from MANIA_WIDTH/MANIA_HEIGHT, so the
// visible image is unaffected by the padding.
//
// NOTE the outer parentheses -- they were missing, and it mattered.
//
// Without them, `MANIA_PITCH * X` parses as `(MANIA_WIDTH + 15) & (0xFFFFFFF0
// * X)` because `*` binds tighter than `&`, which for X = MANIA_HEIGHT *
// sizeof(u16) evaluates to 0. That produced a zero-byte framebuffer
// allocation (so every rasterizer write corrupted the heap, crashing inside
// malloc) and a zero-length present DMA (so the screen stayed black).
//
// The pre-existing uses survived by luck: they either went through the
// screens[0].pitch variable instead of this macro, or combined it with `|`,
// which binds looser than `&` and so happened to group correctly.
#define MANIA_PITCH (((MANIA_WIDTH) + 63) & 0xFFFFFFC0)
#define PSP_SCREEN_WIDTH 480
#define PSP_SCREEN_HEIGHT 272
#define PSP_LINE_SIZE 512

// 1:1 presentation. The game renders 424x240; the display is 480x272. Rather
// than upscaling, the frame is centred and shown at native size.
//
// This exists to delete the GE present blit. That blit -- a bilinear upscale
// of ~130k pixels out of a linear (non-swizzled) VRAM texture -- cost a
// measured, dead-constant 5.27ms per frame in sceGuSync, 25% of the whole
// compute budget, and was the single thing keeping gameplay above the
// 16.67ms needed for 60fps. It isn't filter-bound (nearest saved only
// 0.5ms), so there was no way to make it meaningfully cheaper; the only way
// to get the time back was to stop doing it.
//
// Presenting at native size means the rasterizer's surface can BE the
// display framebuffer's layout -- same 512-word stride, full 272 rows -- so
// the finished frame transfers with one contiguous DMA and the GE is not
// involved in presentation at all. That also removes every display-list
// lifetime and sync-ordering hazard along with it.
// The rasterizer surface uses the engine's own MANIA_PITCH stride and is
// blitted up to fullscreen by the GE, exactly as it was before the 1:1
// experiment. That experiment (display-stride surface DMA'd straight to the
// framebuffer) proved where the time goes, but it costs the fullscreen image,
// so it isn't shippable.
//
// Restoring fullscreen on TOP of a 512-stride surface was attempted and hangs
// the GE at boot -- with the sync in its original place, single-buffered
// lists, vertices back in VRAM, and slack on the allocation, all bisected
// individually. Something about that combination is wrong in a way I could
// not find by elimination, so this is the proven configuration instead.
#define PRESENT_BUFFER_BYTES ((size_t)MANIA_PITCH * MANIA_HEIGHT * sizeof(u16))
// Allocated with extra rows of slack beyond the MANIA_HEIGHT that's actually
// transferred. The tile rasterizers round their span up to whole 16px tiles,
// so they can write a little past size.x; on every row but the last that
// lands harmlessly in the row's padding, but on the last row it would run off
// the end of an exactly-sized allocation and corrupt the heap.
// A couple of rows of slack beyond what's actually transferred: the tile
// rasterizers round their span up to whole 16px tiles, so they can write a
// little past the end of the last row.
#define PRESENT_ALLOC_BYTES ((size_t)MANIA_PITCH * (MANIA_HEIGHT + 2) * sizeof(u16))

#define GE_CMD_FBP    0x9C
#define GE_CMD_FBW    0x9D
#define GE_CMD_TBP0   0xA0
#define GE_CMD_TBW0   0xA8
#define GE_CMD_TSIZE0 0xB8
#define GE_CMD_TFLUSH 0xCB
#define GE_CMD_CLEAR  0xD3
#define GE_CMD_VTYPE  0x12
#define GE_CMD_BASE   0x10
#define GE_CMD_VADDR  0x01
#define GE_CMD_IADDR  0x02
#define GE_CMD_PRIM   0x04
#define GE_CMD_FINISH 0x0F
#define GE_CMD_SIGNAL 0x0C
#define GE_CMD_NOP    0x00
// Render state opcodes, verified by disassembling libpspgu.a rather than from
// memory. Only SHADE was ever wrong: 0x1C is GU_LIGHT1, so the original code
// enabled a light every frame -- which is what left shading flat and put a
// stray coloured rectangle on screen. NOTE sceGuEnable's jump table is laid
// out in REVERSE address order; reading the luis by address instead gives a
// mapping that is wrong for every entry.
//
// Needed by the 3D face list below. The present quad relies on
// Init having set these once and never touches them, so anything the face
// list changes it must also put back.
#define GE_CMD_SHADE    0x50
#define GE_CMD_CULLE    0x1D
#define GE_CMD_TME      0x1E
#define GE_CMD_ABE      0x21
#define GE_CMD_ATE      0x22
#define GE_CMD_ZTE      0x23
#define GE_CMD_SCISSOR1 0xD4
#define GE_CMD_SCISSOR2 0xD5
#define GE_CMD_ZMSK     0xE7
// Terminates a list. NOTE 0x0C, not 0x0B -- 0x0B is RET, and emitting it with
// no matching CALL faults the GE hard enough to power the console off
// (confirmed on hardware). GE_CMD_SIGNAL above is also 0x0C, i.e. it is really
// END -- so the present quad has always been correctly terminated, and the
// "missing END" this was originally added to fix never existed.
#define GE_CMD_END      0x0C

// Present-quad vertices, in main RAM and double-buffered.
//
// These used to live at 0x441FC100 in the uncached VRAM scratch area. An
// earlier attempt at double-buffering put the second copies at 0x441FC200 /
// 0x441FC300 and black-screened -- which I wrongly read as "a list can't be
// double-buffered". The likelier explanation is that sceGuInit reserves VRAM
// for its own use and those addresses collided with it; the original code
// only ever touched 0x441FC000-0x441FC1FF. Keeping both copies in ordinary
// main RAM avoids guessing which VRAM is free at all.
//
// Two copies are needed because FlipScreen no longer waits for the GE before
// returning: the GE can still be drawing last frame's quad while this frame
// builds the next one. Both sit in the VRAM scratch page the original code
// already used (0x441FC000-0x441FC1FF); each needs only 40 bytes.
static float *screen_vertex_buffers[2] = { (float *)0x441FC100, (float *)0x441FC140 };
static int32 present_buffer_index = 0;
#define screen_vertex (screen_vertex_buffers[present_buffer_index])

// The present-quad command list, rebuilt each frame and double-buffered for
// the same reason as the vertices above.
//
// It lives in ordinary cached main RAM rather than the VRAM scratch address
// it originally used, so the ~16 word writes hit cache instead of being
// uncached write-throughs to VRAM. One dcache writeback before
// sceGeListEnQueue publishes it (the GE reads physical memory, so main RAM is
// a perfectly good list source).
static u32 __attribute__((aligned(16))) ge_cmd_buffers[2][64];
#define ge_cmd (ge_cmd_buffers[present_buffer_index])
static u16 *psp_gu_vram_base = (u16 *)(0x44000000);//0x600000
static u32 *ge_cmd_ptr = ge_cmd_buffers[0];
static u32 gecbid;
static u32 video_direct = 0;

// 128 words was fine when this list only ever tracked GU_DIRECT session
// bookkeeping (nothing actually used it -- the present quad is built and
// submitted through its own separate manual ge_cmd/sceGeListEnQueue path).
// Now that GU_DrawSpriteFast issues real sceGu*() calls (draw-buffer/
// offset/viewport/scissor/texture/blend state plus the actual draw) once
// per GPU-eligible sprite, and a frame can have dozens of those, this
// buffer needs real headroom -- 128 words silently overflowed into
// whatever memory followed it, corrupting unrelated state.
// Double-buffered for the same reason as ge_cmd_buffers: FlipScreen starts
// the next list without waiting for the GE to finish the previous one.
static u32 __attribute__((aligned(16))) display_lists[2][16 * 1024];
#define display_list (display_lists[present_buffer_index])

// --- frame timing instrumentation (see GU_UpdateFPSCounter) --------------
// Declared here rather than beside the FPS counter because CopyFrameBuffer,
// further up the file, records into them too.
static SceUInt64 gu_rasterUsecAccum = 0;
// Idle time spent blocked in sceDisplayWaitVblankStart. Subtracting it from
// frame time gives real compute time, which is what determines whether 60fps
// (a <16.67ms compute budget) is reachable at all.
SceUInt64 gu_vblankUsecAccum = 0;
// FlipScreen's own work, excluding the vblank wait.
static SceUInt64 gu_flipUsecAccum   = 0;
static SceUInt64 gu_vblankThisFrame = 0;
// Per-call breakdown. gu_flipSyncUsec now measures the sync in
// CopyFrameBuffer, where the wait for the present blit was moved to.
static SceUInt64 gu_flipEnqUsec = 0, gu_flipFinUsec = 0, gu_flipSyncUsec = 0, gu_flipStartUsec = 0, gu_flipBuildUsec = 0;
// Queue id of the present quad's GE list, so it can be waited on by id.
static int gu_presentListId = -1;
// screen_texture is the VRAM address the GE samples from for the fullscreen
// present quad.
//
// screen_pixels -- what the CPU software rasterizer actually draws into --
// used to alias screen_texture directly, so that CPU draws and (then
// planned) GPU sprite draws would land in the same buffer and keep their
// relative paint order. That alias turned out to be enormously expensive:
// it put the ENTIRE software rasterizer's working surface in VRAM, and CPU
// access to VRAM on PSP is far slower than to main RAM (VRAM is wired for
// GE access; CPU reads/writes take a much longer path). Every sprite pixel,
// FillScreen blend, and circle-outline span was paying that penalty twice,
// once to read and once to write. Direct measurement: FillScreen alone cost
// ~12ms per call -- about 40 CPU cycles for each pixel's three table lookups
// and three adds, which only makes sense if the memory access dominates.
//
// So the rasterizer now draws into main RAM (screen_pixels, allocated in
// Init) and CopyFrameBuffer transfers the finished frame to VRAM once, in
// one linear hardware-DMA burst -- which is exactly what that function's
// name always implied. One bulk sequential transfer replaces ~100k scattered
// slow read-modify-writes per frame.
//
// NOTE for resuming GPU sprite work: the GE can only render into VRAM, so a
// GPU path can no longer assume it shares a buffer with the CPU rasterizer.
// It would have to either draw after this transfer (into screen_texture,
// giving up interleaved ordering) or keep its own VRAM target. Do not simply
// re-alias these to make GPU drawing work -- that reintroduces this cost.
static u16 *screen_texture = (u16 *)(0x4000000 + (512 * 272 * 2));
static u16 *current_screen_texture = (u16 *)(0x4000000 + (512 * 272 * 2));

// VRAM scratch render target for GPU 3D faces.
//
// The GE cannot render into main RAM (pspsdk documents sceGuDrawBuffer's fbp
// as a "VRAM pointer"; on hardware a main-RAM address is silently ignored and
// the GE keeps its previous target). The rasterizer surface lives in main RAM
// because CPU access to VRAM is far slower -- moving it there was the single
// biggest win in the performance work -- so it cannot simply move back.
//
// Instead the frame makes a round trip through here: DMA out, GE draws, DMA
// back. Sits immediately after screen_texture; at 448x240x2 that is 215040
// bytes, leaving ~1.35MB of VRAM still free.
static u16 *gu_3d_scratch = (u16 *)(0x4000000 + (512 * 272 * 2) + (448 * 240 * 2));
static u16 *screen_pixels = NULL;
static u32 screen_pitch = 424;

// --- GPU-accelerated sprite draw path (Milestone 1) ---------------------
// Sprite sheets already live in RAM as raw palette-index bytes at a
// power-of-two width (see LoadSpriteSheet) -- exactly PSP's GU_PSM_T8
// indexed-texture layout, so no reformatting is needed, just a one-time
// VRAM copy per sheet the first time it's drawn through the fast path.
// Sits in the VRAM left over after screen_texture; see Init() for the
// actual base/size (computed from real addresses, not guessed constants).
static u8 *gu_tex_arena      = NULL;
static u32 gu_tex_arena_size = 0;
static u32 gu_tex_arena_used = 0;

struct GUSpriteTex {
    void *vramPixels; // NULL if not (or no longer) resident
    int32 width;
    int32 height;
    uint32 hash[4]; // copy of the owning GFXSurface's hash, to detect slot reuse
};
static GUSpriteTex gfxSurfaceGU[SURFACE_COUNT];

// 16-bit RGBA5551 CLUT rebuilt from a fullPalette bank whenever the active
// bank for a GPU-eligible draw changes. Index 0's alpha is forced to 0 so
// GU_ALPHA_TEST can drop transparent texels -- fullPalette itself has no
// alpha channel (RGB565), so this is the one real conversion GPU sprites
// need that the CPU path doesn't.
static u16 __attribute__((aligned(64))) gu_clut[256];
static int32 gu_clut_bank = -1;

static bool GU_PaletteUniform(int32 y, int32 height, int32 *outBank)
{
    uint8 bank = gfxLineBuffer[y];
    for (int32 i = 1; i < height; ++i) {
        if (gfxLineBuffer[y + i] != bank)
            return false;
    }
    *outBank = bank;
    return true;
}

static void GU_UploadClutForBank(int32 bank)
{
    if (bank == gu_clut_bank)
        return;

    uint16 *pal = fullPalette[bank];
    for (int32 i = 0; i < 256; ++i) {
        uint16 rgb565 = pal[i];
        // This port's RGB565 packing (see rgb32To16_R/G/B in Drawing.cpp) is
        // R in the LOW bits, B in the HIGH bits -- opposite of the usual
        // desktop convention. GU_PSM_5551 follows the same low-to-high
        // R,G,B,A channel order, just with a narrower G and a 1-bit alpha.
        uint16 r5 = rgb565 & 0x1F;         // bits 0-4
        uint16 g6 = (rgb565 >> 5) & 0x3F;  // bits 5-10
        uint16 b5 = (rgb565 >> 11) & 0x1F; // bits 11-15
        uint16 g5 = g6 >> 1;               // 6 bits -> 5 bits for 5551
        // alpha=0 only for index 0, which RSDK always treats as "transparent".
        gu_clut[i] = r5 | (g5 << 5) | (b5 << 10) | (i == 0 ? 0 : (1 << 15));
    }
    sceKernelDcacheWritebackRange(gu_clut, sizeof(gu_clut));
    sceGuClutMode(GU_PSM_5551, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, gu_clut);
    gu_clut_bank = bank;
}

static bool GU_UploadSpriteTexture(uint16 sheetID)
{
    GFXSurface *surface = &gfxSurface[sheetID];
    GUSpriteTex *tex    = &gfxSurfaceGU[sheetID];

    if (tex->vramPixels && HASH_MATCH_MD5(tex->hash, surface->hash))
        return true;

    tex->vramPixels = NULL; // stale: this slot was freed/reloaded since we last cached it

    if (!gu_tex_arena)
        return false;

    // The PSP GE has a hard 512x512 maximum texture size -- some sprite
    // sheets (wide, many-sprite atlases) exceed that, e.g. a 1024x512 sheet
    // found live in the intro sequence (root cause of the flickering/
    // missing-sprite bug this diagnostic build was tracking down: that
    // oversized atlas got fed straight into sceGuTexImage with no bounds
    // check, corrupting whatever it touched). A sheet this large can't be a
    // single GE texture at all, regardless of VRAM budget, so it stays
    // CPU-only permanently for this hash rather than per-draw.
    if (surface->width > 512 || surface->height > 512)
        return false;

    u32 bytes = (u32)surface->width * (u32)surface->height;
    if (gu_tex_arena_used + bytes > gu_tex_arena_size)
        return false; // out of VRAM budget -- this sheet just stays CPU-only

    u8 *dst = gu_tex_arena + gu_tex_arena_used;
    memcpy(dst, surface->pixels, bytes);
    sceKernelDcacheWritebackRange(dst, bytes);
    gu_tex_arena_used += bytes;

    tex->vramPixels = dst;
    tex->width      = surface->width;
    tex->height     = surface->height;
    memcpy(tex->hash, surface->hash, sizeof(tex->hash));
    return true;
}

void RSDK::GU_ClearSpriteTextures()
{
    memset(gfxSurfaceGU, 0, sizeof(gfxSurfaceGU));
    gu_tex_arena_used = 0;
    gu_clut_bank      = -1;
}

struct GUSpriteVertex {
    float u, v;
    float x, y, z;
};

// --- Unified per-frame draw queue (GPU pipeline plan, Stage 0) ----------
// Every sprite AND tile-layer draw for a frame is recorded here, in
// original call order, then replayed in ONE pass at frame end
// (GU_FlushDrawQueue, called from CopyFrameBuffer). Stage 0 replays
// everything through the existing CPU rasterizer (DrawSpriteFlipped_CPU /
// the original DrawLayer* functions) -- functionally a no-op, pure
// plumbing. Later stages add real sceGu*() draws for eligible entries
// without needing to touch this ordering/replay structure again.
//
// Preserving call order matters on its own: the earlier narrow sprite-only
// GPU attempt queued GPU-eligible sprites but drew CPU-fallback ones
// immediately, so the two paths didn't preserve relative draw order --
// a real source of visual corruption, independent of the GE-interleaving
// hang that batching itself fixed.
enum GUQueueEntryType {
    GU_ENTRY_SPRITE,
    GU_ENTRY_LAYER,
    GU_ENTRY_FILLSCREEN,
    GU_ENTRY_RECT,
    GU_ENTRY_ROTOZOOM,
    GU_ENTRY_FACE,
    GU_ENTRY_BLENDEDFACE,
    GU_ENTRY_CIRCLE,
    GU_ENTRY_CIRCLEOUTLINE
};

#define MAX_FACE_VERTS 8

// screen: the ScreenInfo `currentScreen` pointed to when this entry was
// queued (captured, not re-read at flush time) -- ProcessObjectDrawLists
// reassigns the global `currentScreen` per screen index as it walks
// videoSettings.screenCount screens, and can also leave it pointing at a
// screen slot that's momentarily out of sync with screenCount (e.g. across
// an ENGINESTATE_SHOWIMAGE transition, which forces screenCount to 0
// without touching currentScreen). Since queued entries used to be drawn
// immediately, they always saw the *correct* currentScreen for their own
// draw call; deferring them to one shared end-of-frame flush means the
// global may have moved on by the time they replay, so each entry has to
// carry its own screen instead of trusting the global at flush time. This
// was confirmed as the actual root cause of the "Invalid Memory Access"
// crash: a queued entry replayed against screens[1], which is never
// initialized on this single-screen PSP build (garbage pitch/frameBuffer).
// lineBuffer: a snapshot of the global gfxLineBuffer[] (per-scanline active
// palette-bank index, set via SetActivePalette for line-based palette-swap
// effects) at queue time, for the same reason `screen` is captured -- it's
// shared/mutable global state that the next sprite or layer drawn later in
// the same frame can overwrite before this deferred entry actually replays,
// which reads as the wrong palette bank (visually: wrong colors / a
// "warped" look) rather than a crash.
struct GUSpriteEntry {
    ScreenInfo *screen;
    int32 x, y, width, height, sprX, sprY, widthFlip, heightFlip, direction, inkEffect, alpha, sheetID;
    uint8 lineBuffer[SCREEN_YSIZE];
};

// FillScreen is how full-screen fades work (title/logo transitions, pause
// dimming) and is called directly by game object code, not through
// DrawSpriteFlipped -- it has to go through this same queue too, or a fade
// would apply immediately while sprites drawn around it in the same frame
// got deferred, missing whatever hadn't been drawn yet by the time the
// fade "happened". This is exactly the bug seen when the earlier narrow
// GPU attempt didn't preserve relative order.
struct GUFillScreenEntry {
    ScreenInfo *screen; // see GUSpriteEntry::screen
    uint32 color;
    int32 alphaR, alphaG, alphaB;
};

// Tile-layer draws read the single shared `scanlines` buffer, which
// ProcessParallax()/scanlineCallback fills immediately (unchanged, cheap,
// not deferred) right before the original draw call. Since that buffer
// gets overwritten by the NEXT layer before a deferred draw would actually
// run, each queued layer entry snapshots its contents at queue time and
// restores them right before replaying that entry.
//
// Unlike sprites/rects/rotozoom sprites (which clip against currentScreen
// and bake the resulting pixel-space coordinates into the queue entry, so
// they no longer care what currentScreen looks like by flush time), the
// DrawLayer* functions re-derive their render region from
// currentScreen->clipBound_X1/X2/Y1/Y2 (plus pitch/frameBuffer) EVERY time
// they're called -- they were never given clipped coordinates to begin
// with. And clip bounds are genuinely mutated mid-frame: after each draw
// group, ProcessObjectDrawLists resets currentScreen's clip bounds back to
// full-screen (Scene/Object.cpp, right after the per-group layer/entity
// loop). So a layer queued while a clip restriction from an earlier draw
// group was active would, at flush time, replay against whatever the LAST
// draw group left the clip bounds as -- not what was active when it was
// actually queued. A plain screen pointer restore (sufficient for every
// other entry type) doesn't fix this, since the pointer is right but the
// struct's contents have moved on; layers need the whole struct value
// snapshotted, not just a pointer to the (mutable) original. This was the
// actual cause of the water dot-grid artifact: a water layer's rows were
// being computed against post-frame clip bounds instead of its own.
struct GULayerEntry {
    TileLayer *layer;
    ScreenInfo screenSnapshot;
    ScanlineInfo scanlines[MANIA_WIDTH];
    uint8 lineBuffer[SCREEN_YSIZE]; // see GUSpriteEntry::lineBuffer
};

// DrawRectangle is another primitive (dialog/UI panels, debug boxes) called
// directly by game object code like FillScreen, and has to go through this
// same queue for the same reason -- otherwise it draws immediately while a
// FillScreen dim or sprite text queued around it (in the same frame) gets
// deferred, so the dim can end up replaying on top of it and washing it
// out, or text can end up drawn before the panel appears under it.
struct GURectEntry {
    ScreenInfo *screen; // see GUSpriteEntry::screen
    int32 x, y, width, height;
    uint32 color;
    int32 alpha, inkEffect;
};

// DrawSpriteRotozoom (scaled/rotated sprites -- water shimmer, spinning
// logos, other transform-heavy decorations) is another immediate-draw
// primitive that has to go through this queue for the same reason as
// DrawRectangle. Carries the already-computed clip rect and per-row/
// per-pixel transform deltas from DrawSpriteRotozoom rather than the raw
// x/y/pivot/scale/rotation params, since those are all currentScreen-
// independent and only need computing once, at queue time.
struct GURotoEntry {
    ScreenInfo *screen; // see GUSpriteEntry::screen
    int32 left, top, xSize, ySize, fullX, fullY, fullSprX, fullSprY, deltaX, deltaY, deltaXLen, deltaYLen, drawX, drawY, inkEffect, alpha, sheetID;
    uint8 lineBuffer[SCREEN_YSIZE]; // see GUSpriteEntry::lineBuffer
};

// DrawFace/DrawBlendedFace (solid/blended polygon fills -- used by Scene3D
// for pseudo-3D menu decorations like the rotating ring backgrounds, and by
// achievement popups) are two more immediate-draw primitives needing the
// same treatment. Unlike sprites/rects/rotozoom, they re-derive their clip
// region from currentScreen->clipBound_X1/X2/Y1/Y2 at draw time rather than
// baking in pre-clipped coordinates (same as tile layers), so -- also like
// layers -- they need the full ScreenInfo value snapshotted, not just a
// pointer, or they're vulnerable to the same clip-bound-drift bug that
// caused the water dot-grid artifact. The vertex (and per-vertex color)
// arrays are copied rather than referenced by pointer, since the caller's
// array may be entity-local/transient and not survive to flush time.
struct GUFaceEntry {
    ScreenInfo screenSnapshot;
    Vector2 vertices[MAX_FACE_VERTS];
    int32 vertCount, r, g, b, alpha, inkEffect;
};

struct GUBlendedFaceEntry {
    ScreenInfo screenSnapshot;
    Vector2 vertices[MAX_FACE_VERTS];
    uint32 colors[MAX_FACE_VERTS];
    int32 vertCount, alpha, inkEffect;
};

// DrawCircle/DrawCircleOutline -- same clip-bound-drift vulnerability as
// DrawFace (full ScreenInfo snapshot needed, not just a pointer). DrawCircle
// specifically is what drives circular iris-wipe scene transitions -- left
// undeferred, it draws immediately while the FillScreen/sprite content
// around it (already deferred) replays later, so the growing black circle
// either never appears or gets immediately painted over.
struct GUCircleEntry {
    ScreenInfo screenSnapshot;
    int32 x, y, radius;
    uint32 color;
    int32 alpha, inkEffect;
};

struct GUCircleOutlineEntry {
    ScreenInfo screenSnapshot;
    int32 x, y, innerRadius, outerRadius;
    uint32 color;
    int32 alpha, inkEffect;
};

#define GU_LAYER_QUEUE_MAX 32
static GULayerEntry gu_layer_queue[GU_LAYER_QUEUE_MAX];
static int32 gu_layer_queue_count = 0;

struct GUQueueEntry {
    uint8 type;
    union {
        GUSpriteEntry sprite;
        int32 layerIndex; // index into gu_layer_queue[]
        GUFillScreenEntry fillScreen;
        GURectEntry rect;
        GURotoEntry roto;
        GUFaceEntry face;
        GUBlendedFaceEntry blendedFace;
        GUCircleEntry circle;
        GUCircleOutlineEntry circleOutline;
    };
};

#define GU_DRAW_QUEUE_MAX 2048
static GUQueueEntry gu_draw_queue[GU_DRAW_QUEUE_MAX];
static int32 gu_draw_queue_count = 0;

// Queue-pressure diagnostics. gu_queuePeak is the deepest the queue got over
// the logging window; gu_queueDrains counts how many times it filled and had
// to be drained mid-frame. A non-zero drain count means this scene generates
// more than GU_DRAW_QUEUE_MAX draws per frame -- which used to silently
// corrupt draw order, and is the reason the Special Stage rendered wrong.
static int32 gu_queuePeak   = 0;
static int32 gu_queueDrains = 0;

// Per-draw-type time/count accounting, indexed by GUQueueEntryType and
// accumulated across the reporting window. See the flush loop.
#define GU_ENTRY_TYPE_COUNT 9
static SceUInt64 gu_profUsec[GU_ENTRY_TYPE_COUNT];
static uint32 gu_profCount[GU_ENTRY_TYPE_COUNT];

// Set to 1 to write fps.log / fps_history.log / layer_types.log to the game
// directory. Off for normal play: it's a development tool, and there's no
// reason for a build people actually play to be writing to the memstick
// every few seconds. The timing instrumentation itself stays compiled in
// (it's a handful of sceKernelGetSystemTimeWide calls per frame, far below
// measurement noise) so turning this back on is the only step needed to
// profile again.
#define GU_ENABLE_PROFILING 0

// Runtime mirror of the switch above, so the Scene3D timers (in another
// translation unit) can gate on it without needing the macro. With profiling
// off this is a compile-time constant 0 and those timers fold away.
int32 gu_profilingEnabled = GU_ENABLE_PROFILING;

// --- GPU sprite draws (Stage 1a: INK_NONE + FLIP_NONE only) -------------
// Dedicated persistent vertex storage, NOT sceGuGetMemory(). That ring
// buffer is meant for data the GE consumes almost immediately; a whole
// frame's worth of sprites gets batched into one display list and synced
// once (see GU_FlushDrawQueue's end-of-flush cleanup below), so by the time
// the GE actually reads vertex 0, sceGuGetMemory() may have already wrapped
// and overwritten it with a later sprite's data -- a real-hardware-only
// corruption confirmed the first time GPU sprite draws were attempted.
static GUSpriteVertex __attribute__((aligned(16))) gu_vertex_pool[GU_DRAW_QUEUE_MAX][2];
static int32 gu_vertex_pool_used = 0;

// True once this flush has issued at least one real sceGu*() sprite draw --
// tells the end-of-flush cleanup whether there's a batch to close out, and
// GU_TryDrawSpriteGPU whether the draw-buffer/offset/viewport/texture state
// still needs to be set up for the first eligible sprite this frame.
static bool gu_gpu_batch_active = false;

// Points sceGu*() draws at screen_texture (the shared CPU/GPU buffer, see
// its declaration above) instead of the PSP's actual display framebuffer,
// using MANIA_WIDTH/HEIGHT-space offset/viewport/scissor so a sprite's
// already-clipped (x,y,width,height) lines up 1:1 with screen_texture's own
// pixel grid -- the same pattern Init() uses for the present quad, just
// aimed at a different, smaller target. This state is GE-persistent (not
// per-vertex), so it has to be explicitly reset back to the present quad's
// PSP_SCREEN-space values before FlipScreen's raw command list runs, or
// that quad inherits the wrong transform -- see the end-of-flush cleanup in
// GU_FlushDrawQueue.
static void GU_BeginSpriteBatch()
{
    const u32 pitch     = screens[0].pitch;
    // sceGuDrawBuffer's second argument is a BYTE OFFSET from VRAM start,
    // not an absolute pointer -- and Init()'s own sceGuDrawBuffer(...,
    // (void*)0, ...) call (which correctly targets psp_gu_vram_base, the
    // uncached 0x44000000 alias) proves sceGuInit() treats that alias as
    // "offset 0". screen_texture is expressed in the DIFFERENT cached
    // 0x04000000 alias, so its offset from VRAM start has to be computed
    // against THAT base (0x4000000), not against psp_gu_vram_base --
    // subtracting the wrong one (an earlier version of this code did)
    // underflows into a garbage 32-bit value.
    void *fbOffsetBytes = (void *)((u8 *)screen_texture - (u8 *)0x4000000);
    sceGuDrawBuffer(GU_PSM_5650, fbOffsetBytes, pitch);
    sceGuOffset(2048 - (MANIA_WIDTH / 2), 2048 - (MANIA_HEIGHT / 2));
    sceGuViewport(2048, 2048, MANIA_WIDTH, MANIA_HEIGHT);
    sceGuScissor(0, 0, MANIA_WIDTH, MANIA_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    // Indexed/CLUT textures MUST use GU_NEAREST, never GU_LINEAR -- bilinear
    // filtering blends raw palette indices together before the CLUT lookup,
    // which is meaningless and was confirmed to render only sprite outlines
    // with missing interiors last time this was tried.
    sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuEnable(GU_TEXTURE_2D);

    // Alpha-tested replace, no blend -- exactly matches INK_NONE's CPU
    // behavior (skip index-0/transparent texels, everything else opaque).
    // gu_clut already forces index 0's alpha bit to 0, so this alone drops
    // transparent texels with no per-pixel branching needed.
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    sceGuDisable(GU_BLEND);
}

// Attempts to draw one queued sprite entry via the real GPU. Returns false
// (leaving the entry undrawn) for anything outside Stage 1a's narrow scope,
// so the caller falls back to the CPU rasterizer -- exactly the same
// fallback philosophy as every GU_Queue*Draw function above.
static void GU_SyncSpriteBatchIfActive();

// Stage 1a (GPU sprite drawing) is PARKED, disabled by this switch. The
// code below is left intact and working-as-designed on paper, but it is not
// currently correct on real output and must not be enabled without a fresh
// round of verification.
//
// What's established, by direct A/B test: with this at 1 (GPU drawing off,
// everything on the CPU path) the game renders perfectly -- SEGA logo,
// studio logos, no flicker. With it at 0, GPU sprite draws corrupt the
// frame, and crucially they destroy content the CPU drew, including whole
// fullscreen images that never went through the GPU path at all. That
// "GPU draws damage unrelated CPU-drawn pixels" signature is the central
// clue: it means the GE is writing somewhere other than where this code
// intends, so the bug is in the render-target/geometry setup, not in
// per-sprite blending, ordering, or eligibility.
//
// Ruled out by test, so don't re-litigate these:
//   - draw ordering / GPU-vs-CPU z-order (syncing before every CPU draw)
//   - batch size (syncing after every single sprite: no change)
//   - CPU/GPU cache coherency on the shared buffer (writeback+invalidate
//     before the GE executes: no change -- and note PPSSPP's software
//     renderer, where this reproduces, has unified memory and treats the
//     dcache calls as no-ops, so cache cannot be the cause of what's
//     reproducing here)
//   - oversized (>512px) sheets fed to sceGuTexImage: real bug, fixed and
//     kept, but not the cause
//   - frame-buffer stride alignment (432 -> 64-aligned 448: no change)
//
// The methodological mistake worth not repeating: this path was never
// verified in isolation. It went straight to "GPU sprites mixed into a full
// game frame". The next attempt should first prove that a single textured
// quad draws correctly into this buffer with everything else switched off,
// and only then reintroduce mixing.
#define GU_AB_FORCE_CPU_ONLY 1

static bool GU_TryDrawSpriteGPU(GUSpriteEntry *s)
{
#if GU_AB_FORCE_CPU_ONLY
    return false;
#endif

    if (s->inkEffect != INK_NONE || s->direction != FLIP_NONE)
        return false;

    // gfxLineBuffer holds this entry's own snapshot by the time this is
    // called (GU_FlushDrawQueue restores it right before dispatching each
    // sprite entry) -- a sprite whose rows span more than one active
    // palette bank has no single CLUT that's correct for the whole sprite,
    // so it stays CPU-only rather than picking one bank arbitrarily.
    int32 bank;
    if (!GU_PaletteUniform(s->y, s->height, &bank))
        return false;

    if (!GU_UploadSpriteTexture(s->sheetID))
        return false; // sheet not GE-representable, or out of VRAM texture budget

    if (gu_vertex_pool_used >= GU_DRAW_QUEUE_MAX)
        return false; // exhausted this flush's persistent vertex storage

    if (!gu_gpu_batch_active) {
        GU_BeginSpriteBatch();
        gu_gpu_batch_active = true;
    }

    GU_UploadClutForBank(bank);

    GUSpriteTex *tex = &gfxSurfaceGU[s->sheetID];
    sceGuTexImage(0, tex->width, tex->height, tex->width, tex->vramPixels);

    GUSpriteVertex *v = gu_vertex_pool[gu_vertex_pool_used++];
    v[0].u = (float)s->sprX;
    v[0].v = (float)s->sprY;
    v[0].x = (float)s->x;
    v[0].y = (float)s->y;
    v[0].z = 0.0f;
    v[1].u = (float)(s->sprX + s->width);
    v[1].v = (float)(s->sprY + s->height);
    v[1].x = (float)(s->x + s->width);
    v[1].y = (float)(s->y + s->height);
    v[1].z = 0.0f;

    sceKernelDcacheWritebackRange(v, sizeof(GUSpriteVertex) * 2);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    return true;
}

// Retires any pending GPU sprite batch to VRAM right now and restores GE
// state for whatever runs next. No-op if there's no batch open.
//
// This has to run before EVERY CPU-executed draw, not just once at the end
// of the flush loop -- sceGu*() draws are only *recorded* into display_list
// when issued; they don't actually land in VRAM until synced. Every other
// entry type in this queue (tile layers, fills, rects, rotozoom, faces,
// circles, and any sprite GU_TryDrawSpriteGPU rejects) draws synchronously
// straight into VRAM the moment it's called. Without syncing the GPU batch
// out first, a GPU-eligible sprite queued BEFORE one of those CPU draws in
// z-order would still execute AFTER it in real time (whenever the next
// sync happens to land), landing on top of content it was supposed to be
// underneath -- exactly the kind of "flickering / missing depending on
// what else is on screen" symptom this caused the first time GPU sprite
// draws were attempted, and reappeared here even in Stage 1a's much
// narrower single-ink-effect scope, which is what pinned it down to this
// ordering issue rather than a per-blend-mode state leak.
static void GU_SyncSpriteBatchIfActive()
{
    if (!gu_gpu_batch_active)
        return;

    // Restore GE state to exactly what FlipScreen's untracked raw
    // present-quad command list assumes -- it only ever sets
    // FBP/FBW/TBP0/TBW0/TSIZE0 explicitly, never texture format/filter/
    // alpha-test/blend/offset/viewport/scissor, so anything the sprite
    // batch left set would otherwise leak into whatever draws next
    // (including the present quad itself, if this is the flush's last
    // sync -- see the GE state-leak bug from the earlier broad attempt).
    sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR); // must match Init -- see the note there
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH + 1, PSP_SCREEN_HEIGHT + 1);
    gu_clut_bank = -1; // force a re-upload next time a batch opens, regardless of which bank happens to match

    // CPU/GPU cache coherency on the shared framebuffer -- the single most
    // important thing in this function, and the root cause of the
    // "GPU sprites flicker or vanish, CPU sprites always survive" bug.
    //
    // screen_pixels aliases screen_texture through the CACHED VRAM window
    // (0x04000000, see its declaration), so every CPU rasterizer write sits
    // in the CPU data cache as a dirty line. The GE, by contrast, writes
    // VRAM directly, bypassing that cache entirely. Those two views overlap
    // at cache-line granularity (64 bytes = 32 pixels, while a scanline is
    // 424 pixels wide), so any line holding both CPU- and GPU-drawn pixels
    // exists in two conflicting versions at once.
    //
    // The queued CPU draws that ran before this point (tile layers alone
    // cover the whole screen) leave the framebuffer broadly dirty in cache.
    // The GE is about to write its sprites straight to VRAM. Whenever those
    // stale dirty lines are subsequently written back -- CopyFrameBuffer
    // does a blanket writeback of the entire framebuffer right after this
    // flush returns -- they overwrite the GPU's fresh pixels with the
    // pre-GPU CPU contents, erasing exactly the sprites the GE just drew.
    // Which sprites survive depends on which lines happen to still be
    // resident and dirty, hence the flickering; a screen drawn entirely by
    // the GPU over a white FillScreen (the SEGA logo) loses everything and
    // stays blank white.
    //
    // Writing back AND invalidating here, before sceGuFinish lets the GE
    // execute, fixes both directions at once: pending CPU pixels reach VRAM
    // while they're still the correct contents, and dropping the cached
    // copy means any CPU draw later in this same frame re-fetches its lines
    // from VRAM -- so it sees the GPU's pixels and preserves them instead
    // of resurrecting pre-GPU data around its own writes.
    const size_t fbBytes = (size_t)MANIA_HEIGHT * screens[0].pitch * sizeof(u16);
    sceKernelDcacheWritebackInvalidateRange(screen_texture, fbBytes);

    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, display_list);

    gu_gpu_batch_active = false;
    gu_vertex_pool_used = 0; // safe to reuse now -- sceGuSync just proved the GE is done reading it
}

// TEMP A/B: 1 = bypass the Stage 0 draw queue entirely and rasterize every
// draw immediately at call time, i.e. exactly the pre-Stage-0 behavior.
//
// Deferring costs real work per draw that immediate rasterizing doesn't:
// every queued sprite memcpy's a 240-byte gfxLineBuffer snapshot, every
// queued tile layer memcpy's a full ScanlineInfo[MANIA_WIDTH] array plus a
// ScreenInfo, and it all lands in a ~600KB static queue that thrashes the
// PSP's small caches. Stage 0 was verified pixel-identical for CORRECTNESS
// but its performance cost was never measured -- and measured frame time
// (55.9ms) is now roughly double the 15-23ms rendering + ~4ms logic that
// pre-Stage-0 profiling recorded, at a 1.5x HIGHER clock. This switch
// isolates how much of that regression the queue is responsible for.
//
// Only valid while GPU drawing is off (GU_AB_FORCE_CPU_ONLY 1): the queue's
// whole purpose is preserving draw order once GPU and CPU draws mix, so
// bypassing it is only equivalent when everything is on the CPU path.
//
// RESULT: 55.57ms bypassed vs 55.92ms with the queue on -- a 0.6%
// difference, i.e. the queue is NOT a meaningful cost. Left at 0 (queue on,
// normal operation); the switch stays for future A/Bs.
#define GU_BYPASS_DRAW_QUEUE 0

// Defined below, forward-declared so the queue functions can drain the queue
// when it fills. See GU_DrainQueueIfFull.
void GU_FlushDrawQueue();

// What to do when a draw can't be queued -- either the queue is full, or the
// entry is structurally unqueueable (a face with more verts than
// MAX_FACE_VERTS).
//
// Drawing it immediately, which is what this used to do, is WRONG: everything
// queued before it is still sitting in the queue unreplayed, so an immediate
// draw lands underneath draws that were issued before it, and the queued ones
// then paint over it at flush time. That's the exact ordering hazard called
// out in the queue's header comment -- the one the earlier sprite-only GPU
// attempt hit -- reintroduced here in the overflow path.
//
// The fix is to drain first: replaying the queue empties it in call order, so
// by the time the immediate draw (or the freshly-queued entry) happens,
// everything before it has already landed. Order is preserved either way.
//
// This is not hypothetical. 2048 entries is generous for normal gameplay, but
// the Special Stage blows straight through it: every 3D face is one entry, and
// the "View:Special" scene alone is 4096 verts (~1000-1300 faces per
// Draw3DScene) shared by UFO_Circuit, UFO_Decoration, UFO_Player, UFO_Shadow
// and UFO_Springboard, each of which prepares and draws its own batch every
// frame, on top of the usual sprites and layers.
static void GU_DrainQueueIfFull()
{
    if (gu_draw_queue_count >= GU_DRAW_QUEUE_MAX || gu_layer_queue_count >= GU_LAYER_QUEUE_MAX) {
        ++gu_queueDrains;
        GU_FlushDrawQueue();
    }
}

// Queues one sprite draw. If the queue is full it's drained first (see
// GU_DrainQueueIfFull) rather than dropping the draw or reordering it.
void GU_QueueSpriteDraw(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 widthFlip, int32 heightFlip, int32 direction,
                         int32 inkEffect, int32 alpha, int32 sheetID)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawSpriteFlipped_CPU(x, y, width, height, sprX, sprY, widthFlip, heightFlip, direction, inkEffect, alpha, sheetID);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e     = &gu_draw_queue[gu_draw_queue_count++];
    e->type             = GU_ENTRY_SPRITE;
    e->sprite.screen    = currentScreen;
    e->sprite.x         = x;
    e->sprite.y         = y;
    e->sprite.width     = width;
    e->sprite.height    = height;
    e->sprite.sprX      = sprX;
    e->sprite.sprY      = sprY;
    e->sprite.widthFlip  = widthFlip;
    e->sprite.heightFlip = heightFlip;
    e->sprite.direction = direction;
    e->sprite.inkEffect  = inkEffect;
    e->sprite.alpha      = alpha;
    e->sprite.sheetID    = sheetID;
    memcpy(e->sprite.lineBuffer, gfxLineBuffer, SCREEN_YSIZE);
}

static void GU_DrawLayerImmediate(TileLayer *layer)
{
    // One-shot: record which layer types the scene actually uses. The
    // per-type inner loops differ, so this confirms whether an optimization
    // aimed at DrawLayerHScroll is even on the path this scene takes.
    // 0=HSCROLL 1=VSCROLL 2=ROTOZOOM 3=BASIC.
    // (GHZ1 answered: HSCROLL, BASIC and ROTOZOOM; VSCROLL unused.)
#if GU_ENABLE_PROFILING
    {
        static uint32 seenTypes = 0;
        if (layer->type < 32 && !(seenTypes & (1u << layer->type))) {
            seenTypes |= 1u << layer->type;
            FILE *f = fopen("layer_types.log", "a");
            if (f) {
                fprintf(f, "layer type %d in use (0=HSCROLL 1=VSCROLL 2=ROTOZOOM 3=BASIC)\n", layer->type);
                fclose(f);
            }
        }
    }
#endif

    switch (layer->type) {
        case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
        case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
        case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
        case LAYER_BASIC: DrawLayerBasic(layer); break;
        default: break;
    }
}

void RSDK::GU_QueueLayerDraw(TileLayer *layer)
{
#if GU_BYPASS_DRAW_QUEUE
    GU_DrawLayerImmediate(layer);
    return;
#endif

    GU_DrainQueueIfFull();

    GULayerEntry *le    = &gu_layer_queue[gu_layer_queue_count];
    le->layer           = layer;
    le->screenSnapshot  = *currentScreen;
    memcpy(le->scanlines, scanlines, sizeof(ScanlineInfo) * MANIA_WIDTH);
    memcpy(le->lineBuffer, gfxLineBuffer, SCREEN_YSIZE);

    GUQueueEntry *e = &gu_draw_queue[gu_draw_queue_count++];
    e->type         = GU_ENTRY_LAYER;
    e->layerIndex   = gu_layer_queue_count++;
}

// Queues a full-screen fade/dim call. If the queue is full, applies it
// immediately rather than dropping it -- same fallback philosophy as the
// rest of this file.
void GU_QueueFillScreen(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB)
{
#if GU_BYPASS_DRAW_QUEUE
    FillScreen_CPU(color, alphaR, alphaG, alphaB);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e          = &gu_draw_queue[gu_draw_queue_count++];
    e->type                  = GU_ENTRY_FILLSCREEN;
    e->fillScreen.screen     = currentScreen;
    e->fillScreen.color      = color;
    e->fillScreen.alphaR     = alphaR;
    e->fillScreen.alphaG     = alphaG;
    e->fillScreen.alphaB     = alphaB;
}

// Queues a rectangle draw (dialog/UI panels, debug boxes). If the queue is
// full, applies it immediately rather than dropping it -- same fallback
// philosophy as the rest of this file.
void GU_QueueRectDraw(int32 x, int32 y, int32 width, int32 height, uint32 color, int32 alpha, int32 inkEffect)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawRectangle_CPU(x, y, width, height, color, alpha, inkEffect);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e    = &gu_draw_queue[gu_draw_queue_count++];
    e->type            = GU_ENTRY_RECT;
    e->rect.screen     = currentScreen;
    e->rect.x          = x;
    e->rect.y          = y;
    e->rect.width      = width;
    e->rect.height     = height;
    e->rect.color      = color;
    e->rect.alpha      = alpha;
    e->rect.inkEffect  = inkEffect;
}

// Queues a rotozoom (scaled/rotated) sprite draw. If the queue is full,
// applies it immediately rather than dropping it -- same fallback
// philosophy as the rest of this file.
void GU_QueueRotozoomDraw(int32 left, int32 top, int32 xSize, int32 ySize, int32 fullX, int32 fullY, int32 fullSprX, int32 fullSprY, int32 deltaX,
                           int32 deltaY, int32 deltaXLen, int32 deltaYLen, int32 drawX, int32 drawY, int32 inkEffect, int32 alpha, int32 sheetID)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawSpriteRotozoom_CPU(left, top, xSize, ySize, fullX, fullY, fullSprX, fullSprY, deltaX, deltaY, deltaXLen, deltaYLen, drawX, drawY, inkEffect,
                           alpha, sheetID);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e     = &gu_draw_queue[gu_draw_queue_count++];
    e->type             = GU_ENTRY_ROTOZOOM;
    e->roto.screen      = currentScreen;
    e->roto.left        = left;
    e->roto.top         = top;
    e->roto.xSize       = xSize;
    e->roto.ySize       = ySize;
    e->roto.fullX       = fullX;
    e->roto.fullY       = fullY;
    e->roto.fullSprX    = fullSprX;
    e->roto.fullSprY    = fullSprY;
    e->roto.deltaX      = deltaX;
    e->roto.deltaY      = deltaY;
    e->roto.deltaXLen   = deltaXLen;
    e->roto.deltaYLen   = deltaYLen;
    e->roto.drawX       = drawX;
    e->roto.drawY       = drawY;
    e->roto.inkEffect   = inkEffect;
    e->roto.alpha       = alpha;
    e->roto.sheetID     = sheetID;
    memcpy(e->roto.lineBuffer, gfxLineBuffer, SCREEN_YSIZE);
}

// Queues a solid-color polygon fill. If the queue is full, or vertCount
// exceeds what a queue entry can hold (shouldn't happen -- these are all
// small triangles/quads in practice), applies it immediately rather than
// dropping it -- same fallback philosophy as the rest of this file.
// NOTE: param order here is (b, g, r), matching DrawFace's own (unusual but
// pre-existing) parameter order -- kept consistent end-to-end so nothing
// needs reordering at the flush call site either.
void GU_QueueFaceDraw(Vector2 *vertices, int32 vertCount, int32 b, int32 g, int32 r, int32 alpha, int32 inkEffect)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawFace_CPU(vertices, vertCount, b, g, r, alpha, inkEffect);
    return;
#endif

    if (vertCount > MAX_FACE_VERTS) {
        // Too many verts to fit an entry -- drain first so this still lands in
        // call order rather than underneath everything already queued.
        GU_FlushDrawQueue();
        DrawFace_CPU(vertices, vertCount, b, g, r, alpha, inkEffect);
        return;
    }

    GU_DrainQueueIfFull();

    GUQueueEntry *e         = &gu_draw_queue[gu_draw_queue_count++];
    e->type                 = GU_ENTRY_FACE;
    e->face.screenSnapshot  = *currentScreen;
    e->face.vertCount       = vertCount;
    e->face.b               = b;
    e->face.g               = g;
    e->face.r               = r;
    e->face.alpha           = alpha;
    e->face.inkEffect       = inkEffect;
    memcpy(e->face.vertices, vertices, sizeof(Vector2) * vertCount);
}

// Queues a per-vertex-blended polygon fill. Same fallback philosophy as
// GU_QueueFaceDraw.
void GU_QueueBlendedFaceDraw(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawBlendedFace_CPU(vertices, colors, vertCount, alpha, inkEffect);
    return;
#endif

    if (vertCount > MAX_FACE_VERTS) {
        // Too many verts to fit an entry -- drain first so this still lands in
        // call order rather than underneath everything already queued.
        GU_FlushDrawQueue();
        DrawBlendedFace_CPU(vertices, colors, vertCount, alpha, inkEffect);
        return;
    }

    GU_DrainQueueIfFull();

    GUQueueEntry *e                  = &gu_draw_queue[gu_draw_queue_count++];
    e->type                          = GU_ENTRY_BLENDEDFACE;
    e->blendedFace.screenSnapshot    = *currentScreen;
    e->blendedFace.vertCount         = vertCount;
    e->blendedFace.alpha             = alpha;
    e->blendedFace.inkEffect         = inkEffect;
    memcpy(e->blendedFace.vertices, vertices, sizeof(Vector2) * vertCount);
    memcpy(e->blendedFace.colors, colors, sizeof(uint32) * vertCount);
}

// Queues a solid-color filled circle (iris-wipe transitions, etc). Same
// fallback philosophy as the rest of this file.
void GU_QueueCircleDraw(int32 x, int32 y, int32 radius, uint32 color, int32 alpha, int32 inkEffect)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawCircle_CPU(x, y, radius, color, alpha, inkEffect);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e             = &gu_draw_queue[gu_draw_queue_count++];
    e->type                     = GU_ENTRY_CIRCLE;
    e->circle.screenSnapshot    = *currentScreen;
    e->circle.x                 = x;
    e->circle.y                 = y;
    e->circle.radius            = radius;
    e->circle.color             = color;
    e->circle.alpha             = alpha;
    e->circle.inkEffect         = inkEffect;
}

// Queues a ring/donut outline circle. Same fallback philosophy as the rest
// of this file.
void GU_QueueCircleOutlineDraw(int32 x, int32 y, int32 innerRadius, int32 outerRadius, uint32 color, int32 alpha, int32 inkEffect)
{
#if GU_BYPASS_DRAW_QUEUE
    DrawCircleOutline_CPU(x, y, innerRadius, outerRadius, color, alpha, inkEffect);
    return;
#endif

    GU_DrainQueueIfFull();

    GUQueueEntry *e                    = &gu_draw_queue[gu_draw_queue_count++];
    e->type                            = GU_ENTRY_CIRCLEOUTLINE;
    e->circleOutline.screenSnapshot    = *currentScreen;
    e->circleOutline.x                 = x;
    e->circleOutline.y                 = y;
    e->circleOutline.innerRadius       = innerRadius;
    e->circleOutline.outerRadius       = outerRadius;
    e->circleOutline.color             = color;
    e->circleOutline.alpha             = alpha;
    e->circleOutline.inkEffect         = inkEffect;
}

// --- GPU 3D faces (Stage A: can the GE draw into our framebuffer at all) ---
//
// Set to 1 to draw one fixed test triangle at the end of every flush. This
// exists to isolate ONE question -- can the GE render into the main-RAM
// rasterizer surface, with the right address, stride, vertex format and
// cache handling -- from all the batching/ordering logic that comes after.
// The parked sprite attempt failed on exactly this layer and the batching
// on top made it far harder to see. Keep at 0 unless bringing that up.
#define GU_3D_TEST_TRIANGLE 0

// Gouraud 2D triangle vertex: GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D.
//
// The GE requires the vertex stride to be a multiple of its largest member,
// so this is padded to 12 bytes rather than the 10 the fields occupy.
// Component order is fixed by the hardware: colour first, then position.
struct GUFaceVertex {
    u32 color;      // 0xAABBGGRR -- red in the LOW byte, same order as the framebuffer
    s16 x, y, z;
    s16 pad;
};


// Replays every queued entry in original order. Called once per frame from
// CopyFrameBuffer(), after the CPU-side writeback. Stage 0: every entry is
// a CPU-fallback replay, so this is equivalent to what used to happen
// inline during ProcessObjectDrawLists/game object draw callbacks, just
// deferred to one place.
void GU_FlushDrawQueue()
{
    // Each entry carries the ScreenInfo `currentScreen` (see
    // GUSpriteEntry::screen) and, where relevant, the gfxLineBuffer[]
    // palette-bank snapshot (see GUSpriteEntry::lineBuffer) captured at
    // queue time -- both are live globals that can be overwritten by other
    // draws later in the same frame before a deferred entry actually
    // replays, so each entry restores its own copy instead of trusting
    // whatever the global holds at flush time. Both are restored to their
    // real end-of-frame values once the whole queue has been replayed.
    ScreenInfo *realCurrentScreen = currentScreen;
    uint8 realLineBuffer[SCREEN_YSIZE];
    memcpy(realLineBuffer, gfxLineBuffer, SCREEN_YSIZE);

    // A first, broadly-scoped attempt at real sceGu*() GPU sprite draws
    // (multiple flip directions and ink effects added together) surfaced a
    // chain of genuine but increasingly obscure GE/emulator issues here (a
    // GE texture-mode state leak into FlipScreen's present quad, bilinear
    // filtering on indexed/CLUT textures, PPSSPP's buffered-rendering
    // backend not syncing plain CPU writes into a GPU render target's
    // cache, a real-hardware-only sceGuGetMemory ring-buffer wraparound
    // corrupting earlier sprites in a large batch, and an unresolved
    // flickering bug) each of which took real effort to isolate. It was
    // reverted and is being rebuilt as a sequence of much smaller,
    // independently-verified micro-stages instead -- see
    // GU_TryDrawSpriteGPU above, currently scoped to Stage 1a
    // (INK_NONE + FLIP_NONE only). Every other entry type below is still
    // the plain CPU-fallback replay this queue always guarantees correct
    // ordering for, regardless of what becomes GPU-accelerated.
    for (int32 i = 0; i < gu_draw_queue_count; ++i) {
        GUQueueEntry *e = &gu_draw_queue[i];

        // Every entry type except a GPU-eligible sprite draws synchronously
        // on the CPU -- see GU_SyncSpriteBatchIfActive's comment for why a
        // pending GPU batch has to be retired before any of those run.
        if (e->type != GU_ENTRY_SPRITE)
            GU_SyncSpriteBatchIfActive();

        // Per-draw-type cost accounting. The entire GPU acceleration premise
        // depends on knowing WHICH draws actually cost the frame, and
        // measured frame time (~56ms) is far above what earlier (since
        // deleted) profiling implied -- so measure it rather than assume.
        const SceUInt64 gu_entryStart = sceKernelGetSystemTimeWide();
        gu_profCount[e->type]++;

        switch (e->type) {
            case GU_ENTRY_SPRITE: {
                GUSpriteEntry *s = &e->sprite;
                if (!s->screen) break;
                currentScreen = s->screen;
                memcpy(gfxLineBuffer, s->lineBuffer, SCREEN_YSIZE);
                if (!GU_TryDrawSpriteGPU(s)) {
                    GU_SyncSpriteBatchIfActive();
                    DrawSpriteFlipped_CPU(s->x, s->y, s->width, s->height, s->sprX, s->sprY, s->widthFlip, s->heightFlip, s->direction,
                                          s->inkEffect, s->alpha, s->sheetID);
                }
                break;
            }
            case GU_ENTRY_LAYER: {
                if (e->layerIndex < 0 || e->layerIndex >= gu_layer_queue_count) break;
                GULayerEntry *le  = &gu_layer_queue[e->layerIndex];
                ScreenInfo snapshotScreen = le->screenSnapshot;
                currentScreen             = &snapshotScreen;
                memcpy(scanlines, le->scanlines, sizeof(ScanlineInfo) * MANIA_WIDTH);
                memcpy(gfxLineBuffer, le->lineBuffer, SCREEN_YSIZE);
                GU_DrawLayerImmediate(le->layer);
                break;
            }
            case GU_ENTRY_FILLSCREEN:
                if (!e->fillScreen.screen) break;
                currentScreen = e->fillScreen.screen;
                FillScreen_CPU(e->fillScreen.color, e->fillScreen.alphaR, e->fillScreen.alphaG, e->fillScreen.alphaB);
                break;
            case GU_ENTRY_RECT:
                if (!e->rect.screen) break;
                currentScreen = e->rect.screen;
                DrawRectangle_CPU(e->rect.x, e->rect.y, e->rect.width, e->rect.height, e->rect.color, e->rect.alpha, e->rect.inkEffect);
                break;
            case GU_ENTRY_ROTOZOOM: {
                GURotoEntry *r = &e->roto;
                if (!r->screen) break;
                currentScreen = r->screen;
                memcpy(gfxLineBuffer, r->lineBuffer, SCREEN_YSIZE);
                DrawSpriteRotozoom_CPU(r->left, r->top, r->xSize, r->ySize, r->fullX, r->fullY, r->fullSprX, r->fullSprY, r->deltaX, r->deltaY,
                                       r->deltaXLen, r->deltaYLen, r->drawX, r->drawY, r->inkEffect, r->alpha, r->sheetID);
                break;
            }
            case GU_ENTRY_FACE: {
                GUFaceEntry *f            = &e->face;
                ScreenInfo snapshotScreen = f->screenSnapshot;
                currentScreen             = &snapshotScreen;
                DrawFace_CPU(f->vertices, f->vertCount, f->b, f->g, f->r, f->alpha, f->inkEffect);
                break;
            }
            case GU_ENTRY_BLENDEDFACE: {
                GUBlendedFaceEntry *f     = &e->blendedFace;
                ScreenInfo snapshotScreen = f->screenSnapshot;
                currentScreen             = &snapshotScreen;
                DrawBlendedFace_CPU(f->vertices, f->colors, f->vertCount, f->alpha, f->inkEffect);
                break;
            }
            case GU_ENTRY_CIRCLE: {
                GUCircleEntry *c          = &e->circle;
                ScreenInfo snapshotScreen = c->screenSnapshot;
                currentScreen             = &snapshotScreen;
                DrawCircle_CPU(c->x, c->y, c->radius, c->color, c->alpha, c->inkEffect);
                break;
            }
            case GU_ENTRY_CIRCLEOUTLINE: {
                GUCircleOutlineEntry *c   = &e->circleOutline;
                ScreenInfo snapshotScreen = c->screenSnapshot;
                currentScreen             = &snapshotScreen;
                DrawCircleOutline_CPU(c->x, c->y, c->innerRadius, c->outerRadius, c->color, c->alpha, c->inkEffect);
                break;
            }
            default: break;
        }

        gu_profUsec[e->type] += sceKernelGetSystemTimeWide() - gu_entryStart;
    }

    // Catches a GPU batch left open because the frame's last queued entry
    // was itself a GPU-eligible sprite (the common case, in practice, given
    // most frames end on foreground sprites) -- also what keeps this
    // frame's GPU draws from showing up one frame late against
    // FlipScreen's present quad, which doesn't sync display_list itself
    // until after submitting its own raw command list.
    GU_SyncSpriteBatchIfActive();


    currentScreen = realCurrentScreen;
    memcpy(gfxLineBuffer, realLineBuffer, SCREEN_YSIZE);

    if (gu_draw_queue_count > gu_queuePeak)
        gu_queuePeak = gu_draw_queue_count;

    gu_draw_queue_count  = 0;
    gu_layer_queue_count = 0;
}

#define GE_CMD(cmd, operand)                                                \
  *ge_cmd_ptr = (((GE_CMD_##cmd) << 24) | (operand));                       \
  ge_cmd_ptr++                                                              \

static void Ge_Finish_Callback(int id, void *arg)
{
}

// --- GPU 3D faces, via the raw GE queue --------------------------------
//
// Built and submitted exactly like FlipScreen's present quad: raw GE command
// words, sceGeListEnQueue, waited on by queue id. NOT sceGuStart/Finish/Sync.
//
// That distinction is the whole reason this works. Init() closes its setup
// list and leaves none open, and the present path deliberately bypasses the
// GU driver -- see the note in FlipScreen about its list machinery being
// "exactly what black-screens when the sync is deferred". Every earlier
// attempt here drove the GE through sceGu*() calls and corrupted the present
// quad, which showed up as the title screen cropped into a corner.
//
// Own command buffer and pointer: ge_cmd_ptr belongs to FlipScreen, so this
// saves and restores it rather than sharing.
static u32 __attribute__((aligned(16))) ge_tri_cmd[64];
static GUFaceVertex __attribute__((aligned(16))) ge_tri_verts[3];

#if GU_3D_TEST_TRIANGLE
// One obvious triangle, gouraud-shaded red/green/blue, drawn through a VRAM
// scratch target.
//
// The GE CANNOT render into main RAM. pspsdk documents sceGuDrawBuffer's fbp
// as a "VRAM pointer", and on hardware a main-RAM address is not rejected --
// it is silently ignored, leaving the GE drawing into whatever VRAM target
// was set previously. Measured directly rather than inferred: with FBP/FBW
// pointing at screen_pixels, zero pixels changed in it while triangles were
// visibly on screen. Every earlier failure here (flicker, repeats, banding)
// was a symptom of that one fact.
//
// So the surface makes a round trip: the finished CPU frame is DMA'd out to
// VRAM, the GE draws into it there, and the result comes back. Two ~215KB
// sceDmacMemcpy transfers, well under a millisecond, against the ~11ms the
// CPU currently spends rasterizing 3D faces.
static void GU_Draw3DTestTriangleRaw()
{
    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);

    // Colours are 0xAABBGGRR -- red in the LOW byte, matching the
    // framebuffer's PSP-native channel order.
    ge_tri_verts[0].color = 0xFF0000FF; ge_tri_verts[0].x = 60;  ge_tri_verts[0].y = 40;  // red
    ge_tri_verts[1].color = 0xFF00FF00; ge_tri_verts[1].x = 200; ge_tri_verts[1].y = 60;  // green
    ge_tri_verts[2].color = 0xFFFF0000; ge_tri_verts[2].x = 100; ge_tri_verts[2].y = 180; // blue
    for (int32 i = 0; i < 3; ++i) { ge_tri_verts[i].z = 0; ge_tri_verts[i].pad = 0; }

    // Snapshot a scanline through the middle of the triangle so the result
    // can be confirmed in memory rather than from a photograph.
    static u16 dbgRowBefore[512];
    const int32 dbgY = 100; // triangle spans y=40..180
    memcpy(dbgRowBefore, screen_pixels + (size_t)dbgY * pitch, pitch * sizeof(u16));

    // Push the CPU's pixels out of the data cache before the DMA reads them.
    // Whole-cache, not the range calls -- those were not reliably covering
    // the 215KB surface and left the output banded.
    sceKernelDcacheWritebackInvalidateAll();

    // Hand the finished CPU frame to VRAM, where the GE can actually draw.
    sceDmacMemcpy(gu_3d_scratch, screen_pixels, bytes);

    const u32 target = (u32)gu_3d_scratch | 0x40000000; // uncached VRAM alias

    u32 *saved_ptr = ge_cmd_ptr;
    ge_cmd_ptr     = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);

    // Untextured, unblended, no depth, no culling. Culling in particular must
    // stay off: Draw3DScene resolves visibility by sorting faces back to
    // front, so triangles arrive in both windings.
    GE_CMD(TME, 0);
    GE_CMD(ABE, 0);
    GE_CMD(ATE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1); // no depth buffer is ever established -- don't write one
    GE_CMD(CULLE, 0);
    GE_CMD(SHADE, 1); // gouraud

    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, ((MANIA_HEIGHT - 1) << 10) | (MANIA_WIDTH - 1));

    // Colour 8888 (7<<2), position 16-bit (2<<7), through/2D transform (1<<23).
    GE_CMD(VTYPE, (1 << 23) | (2 << 7) | (7 << 2));
    GE_CMD(BASE, ((u32)ge_tri_verts & 0xFF000000) >> 8);
    GE_CMD(VADDR, (u32)ge_tri_verts & 0x00FFFFFF);
    GE_CMD(PRIM, (3 << 16) | 3); // GU_TRIANGLES, 3 vertices

    // Put back what FlipScreen's present list relies on but never sets.
    GE_CMD(TME, 1);
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);

    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    const int32 cmdWords = (int32)(ge_cmd_ptr - ge_tri_cmd);
    ge_cmd_ptr           = saved_ptr;

    // The GE reads both of these straight out of memory.
    sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
    sceKernelDcacheWritebackRange(ge_tri_verts, sizeof(ge_tri_verts));

    const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
    if (qid >= 0)
        sceGeListSync(qid, 0);

    // Bring the composited result back, so the rest of the frame's CPU draws
    // and the present DMA both see it.
    sceDmacMemcpy(screen_pixels, gu_3d_scratch, bytes);
    sceKernelDcacheWritebackInvalidateAll();

    {
        static int32 dbgDone = 0;
        if (!dbgDone) {
            dbgDone      = 1;
            const u16 *r = screen_pixels + (size_t)dbgY * pitch;
            FILE *df     = fopen("tri_dbg.log", "w");
            if (df) {
                fprintf(df, "scratch = %p   GE target = 0x%08X\n", (void *)gu_3d_scratch, (unsigned)target);
                fprintf(df, "pitch = %d   list words = %d\n", (int)pitch, (int)cmdWords);
                fprintf(df, "expected: ONE run near x=77..167\n\n");
                int32 runs = 0, start = -1;
                for (int32 x = 0; x <= (int32)pitch; ++x) {
                    const int32 changed = (x < (int32)pitch) && (r[x] != dbgRowBefore[x]);
                    if (changed && start < 0)
                        start = x;
                    else if (!changed && start >= 0) {
                        ++runs;
                        if (runs <= 16)
                            fprintf(df, "run %2d: x=%3d..%3d len=%3d  first=0x%04X mid=0x%04X last=0x%04X\n", (int)runs, (int)start,
                                    (int)(x - 1), (int)(x - start), (unsigned)r[start], (unsigned)r[(start + x - 1) / 2], (unsigned)r[x - 1]);
                        start = -1;
                    }
                }
                fprintf(df, "\ntotal runs on this scanline: %d\n", (int)runs);
                fclose(df);
            }
        }
    }
}
#endif

#define get_screen_pixels()                                                 \
  screen_pixels                                                             \

#define get_screen_pitch()                                                  \
  screen_pitch                                                              \

bool RenderDevice::Init()
{//This is just gpSP display code atm...
  // The PSP boots at a conservative default clock unless a game explicitly asks
  // for the max; without this the CPU/bus run well below their real ceiling.
  // 333/333/166 is the hardware maximum and what demanding titles/ports use.
  // This matters more here than on a typical port: rendering is a CPU software
  // rasterizer, so frame time scales close to directly with CPU and bus clock.
  // Costs battery life and runs warmer than the 222/222/111 stock clock.
  scePowerSetClockFrequency(333, 333, 166);

printf("Mania Pitch is %i",MANIA_PITCH);
  sceDisplaySetMode(0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
  sceDisplayWaitVblankStart();
  sceDisplaySetFrameBuf((void*)psp_gu_vram_base, PSP_LINE_SIZE,
  PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);

  sceGuInit();

  sceGuStart(GU_DIRECT, display_list);
  sceGuDrawBuffer(GU_PSM_5650, (void*)0, PSP_LINE_SIZE);
  sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
   (void*)0, PSP_LINE_SIZE);
  sceGuClear(GU_COLOR_BUFFER_BIT);

  sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
  sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);

  sceGuScissor(0, 0, PSP_SCREEN_WIDTH + 1, PSP_SCREEN_HEIGHT + 1);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  // GU_LINEAR. Switching the present blit to GU_NEAREST was tried, on the
  // theory that bilinear's four texel fetches per output pixel were what
  // made the blit (and so the sceGuSync waiting on it) cost 5.6ms/frame.
  // Measured on hardware it saved only 0.5ms -- the blit is not texture-
  // fetch bound -- while making the 424x240 -> 480x272 upscale visibly
  // worse. Not a trade worth making; don't repeat it.
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  sceGuEnable(GU_TEXTURE_2D);

  sceGuFrontFace(GU_CW);
  sceGuDisable(GU_BLEND);

  sceGuFinish();
  sceGuSync(0, 0);

  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);

  PspGeCallbackData gecb;
  gecb.signal_func = NULL;
  gecb.signal_arg = NULL;
  gecb.finish_func = Ge_Finish_Callback;
  gecb.finish_arg = NULL;
  gecbid = sceGeSetCallback(&gecb);

  // Both copies -- the present quad alternates between them each frame (see
  // screen_vertex_buffers). The contents are identical and never change;
  // they're duplicated only so the GE can still be reading one while the CPU
  // is free to touch the other.
  for (int32 v = 0; v < 2; ++v) {
    float *sv = screen_vertex_buffers[v];
    sv[0] = 0 + 0.5;
    sv[1] = 0 + 0.5;
    sv[2] = 0 + 0.5;
    sv[3] = 0 + 0.5;
    sv[4] = 0;
    sv[5] = MANIA_WIDTH - 0.5;
    sv[6] = MANIA_HEIGHT - 0.5;
    sv[7] = PSP_SCREEN_WIDTH - 0.5;
    sv[8] = PSP_SCREEN_HEIGHT - 0.5;
    sv[9] = 0;
  }


  // The present-quad GE command sequence used to be built once here and
  // replayed unchanged every frame via sceGeListEnQueue (fine when the only
  // thing the GE ever draws is this one fixed quad). It's now rebuilt fresh
  // each frame in FlipScreen() instead -- a hard prerequisite for later
  // adding a variable number of GPU-accelerated sprite draws per frame,
  // which can't be part of a precomputed static list. See FlipScreen().

  // dummy out later maybe possibly
  printf("RSDKv5 PSP: RenderDevice init\n");
  printf("The programmer has a nap. Hold out!\nProgrammer!\n");


  scanlines = (ScanlineInfo*) malloc(MANIA_WIDTH * sizeof(ScanlineInfo));
  if (!scanlines)
  return false;

  engine.inFocus = 1;
  videoSettings.windowState = WINDOWSTATE_ACTIVE;
  videoSettings.dimMax = 1.0;
  videoSettings.dimPercent = 1.0;

  RSDK::SetScreenSize(0, MANIA_WIDTH,MANIA_HEIGHT );

  // The rasterizer's working surface lives in MAIN RAM, not VRAM (see the
  // comment on screen_texture's declaration for the measurements behind
  // that), and is laid out exactly like the display framebuffer: 512-word
  // stride, 272 rows. That lets the finished frame reach the display in one
  // contiguous DMA with no GE blit -- see PRESENT_X_OFFSET.
  //
  // Overriding pitch to the display's 512 (SetScreenSize computes 432) is
  // what makes the strides match. The engine keeps drawing 424x240; only the
  // row stride changes, which every rasterizer already reads from
  // currentScreen->pitch.
  // Engine default stride (MANIA_PITCH). Not overridden -- see PRESENT_BUFFER_BYTES.

  // 64-byte aligned: 16 is enough for sceDmacMemcpy, but the GE also renders
  // into this surface, and the dcache range calls that keep CPU and GE views
  // coherent operate on whole 64-byte cache lines. With a merely 16-aligned
  // base, the first and last lines of the range are only partially covered
  // and are not invalidated -- so stale CPU pixels survive there and get
  // written back over the GE's output. A cache line is 32 pixels wide, which
  // is exactly why that showed up as a triangle sliced into 32px vertical
  // bands, flickering as which lines happened to still be resident changed.
  // PRESENT_ALLOC_BYTES is already a multiple of 64.
  screen_pixels = (u16 *)memalign(64, PRESENT_ALLOC_BYTES);
  if (!screen_pixels)
    return false;

  memset(screen_pixels, 0, PRESENT_ALLOC_BYTES);
  screens[0].frameBuffer = screen_pixels;

#if GU_3D_TEST_TRIANGLE
  // What the GE is actually being handed. The test triangle rendering as ~7
  // side-by-side copies squashed into a band points at a row stride of 64
  // rather than 448 (448/64 == 7), so dump the real values rather than
  // assume where that comes from.
  {
      FILE *df = fopen("gu_dbg.log", "w");
      if (df) {
          const u32 geTarget = (u32)screen_pixels | 0x40000000; // uncached alias, as handed to the GE
          const u32 fbp      = geTarget & 0x00FFFFFF;
          const u32 fbw      = ((geTarget & 0xFF000000) >> 8) | (u32)screens[0].pitch;
          fprintf(df, "screen_pixels = %p\n", (void *)screen_pixels);
          fprintf(df, "screens[0].pitch = %d\n", (int)screens[0].pitch);
          fprintf(df, "screens[0].size  = %d x %d\n", (int)screens[0].size.x, (int)screens[0].size.y);
          fprintf(df, "MANIA_PITCH = %d   MANIA_WIDTH = %d\n", (int)MANIA_PITCH, (int)MANIA_WIDTH);
          fprintf(df, "PRESENT_BUFFER_BYTES = %u\n", (unsigned)PRESENT_BUFFER_BYTES);
          fprintf(df, "screen_texture = %p\n", (void *)screen_texture);
          fprintf(df, "GE FBP word = 0x%08X\n", (unsigned)fbp);
          fprintf(df, "GE FBW word = 0x%08X  (low 16 = width = %u)\n", (unsigned)fbw, (unsigned)(fbw & 0xFFFF));
          fprintf(df, "base 64-aligned: %s\n", (((u32)screen_pixels & 63) == 0) ? "yes" : "NO");
          fclose(df);
      }
  }
#endif

  // GU sprite texture arena: whatever VRAM is left after screen_texture,
  // minus a safety margin. Computed from real addresses/sizes rather than a
  // guessed constant so it can't silently overrun VRAM if any of those
  // change later.
  {
      const size_t screenTexBytes = (size_t)MANIA_HEIGHT * screens[0].pitch * sizeof(u16);
      // Skip past the 3D scratch target, which sits directly after screen_texture.
      gu_tex_arena = (u8 *)screen_texture + screenTexBytes * 2;

      const size_t vramTotal   = 2 * 1024 * 1024;
      const size_t vramCached  = 0x04000000;
      const size_t usedBefore  = (size_t)((u8 *)gu_tex_arena - (u8 *)vramCached);
      const size_t safetyMargin = 256 * 1024;

      gu_tex_arena_size = (usedBefore + safetyMargin < vramTotal)
                               ? (u32)(vramTotal - usedBefore - safetyMargin)
                               : 0;
      gu_tex_arena_used = 0;
  }

  InitInputDevices();
  if (!AudioDevice::Init())
  return false;

  return true;
}

void clear_screen(u16 color)
{
  u32 i;
  u16 *src_ptr = get_screen_pixels();

  sceGuSync(0, 0);

  for(i = 0; i < (MANIA_PITCH * MANIA_HEIGHT); i++, src_ptr++)
  {
    *src_ptr = color;
  }

}

// Defined with the FPS counter further down. NOTE: these bracket
// CopyFrameBuffer, which is where ALL pixel work happens while the draw
// queue is on. With GU_BYPASS_DRAW_QUEUE the rasterizing instead happens
// inline during the game's own draw calls (before CopyFrameBuffer is ever
// reached), so `raster` reads near zero there and only total frame time is
// comparable between the two -- which is the number the A/B turns on anyway.
void GU_MarkRasterStart();
void GU_MarkRasterEnd();

void RenderDevice::CopyFrameBuffer()
{
  GU_MarkRasterStart();

  // Replay every sprite/tile-layer draw queued this frame, in original call
  // order, right here -- see GU_FlushDrawQueue() for why this has to be a
  // single contained pass rather than scattered individual draws. This is
  // where the actual CPU pixel writes for the frame happen now that draws
  // are deferred (Stage 0), so it must run BEFORE the transfer below.
  GU_FlushDrawQueue();

#if GU_3D_TEST_TRIANGLE
  // Here, NOT at the end of GU_FlushDrawQueue. That function is no longer
  // called once per frame: every queue drain runs it, and so does each of
  // DrawLine/DrawTile/DrawDeformedSprite/DrawDevString, which drain the
  // queue before writing straight to the framebuffer. Drawing the test
  // triangle there produced one copy per flush -- seven of them on the title
  // screen. This spot runs exactly once per frame, after all CPU pixel work
  // and before the surface is written back and DMA'd out.
  GU_Draw3DTestTriangleRaw();
#endif

  // The rasterizer's writes are sitting in the CPU data cache, so they have
  // to be written back before the DMA engine -- which reads memory directly,
  // with no view of that cache -- transfers the buffer, or stale pixels
  // would be sent.
  //
  sceKernelDcacheWritebackRange(screen_pixels, PRESENT_BUFFER_BYTES);

  // This is the one point in the frame that genuinely needs the previous
  // frame's present blit to be finished: the DMA below overwrites
  // screen_texture, which is what the GE samples. FlipScreen deliberately
  // does not wait (see the note there) -- by now the GE has had all of
  // ProcessObjects and the whole draw-queue flush to finish in parallel, so
  // this should usually return immediately rather than stalling ~5ms.
  // The GE is (or was) reading screen_texture for the previous frame's
  // present quad, and the DMA below overwrites it -- this is the one point in
  // the frame that genuinely requires that blit to be finished. FlipScreen
  // deliberately no longer waits, so by now the GE has had all of
  // ProcessObjects plus the whole draw-queue flush to work in parallel: the
  // ~5.6ms blit overlaps the frame's CPU work instead of blocking it.
  {
      const SceUInt64 t0 = sceKernelGetSystemTimeWide();
      if (gu_presentListId >= 0)
          sceGeListSync(gu_presentListId, 0); // 0 = wait for completion
      gu_flipSyncUsec += sceKernelGetSystemTimeWide() - t0;
  }

  sceDmacMemcpy(screen_texture, screen_pixels, PRESENT_BUFFER_BYTES);

  // All of this frame's pixel work is done as of here -- see
  // GU_UpdateFPSCounter for how this splits the frame into raster vs. rest.
  GU_MarkRasterEnd();
}

void RGBtoBGR()
{
        int32 cnt = (MANIA_WIDTH+16) * MANIA_HEIGHT;
        for (int32 id = 0; cnt > 0; --cnt, ++id) {
            uint16 px = screens[0].frameBuffer[id];
            screens[0].frameBuffer[id] = ((px & 0x1F)<< 11) | (px & 0x7E0) | ((px & 0xF800) >> 11);
        }
}


// Coarse, self-contained FPS readout for the GPU pipeline plan's staged
// rollout (the earlier frame_profile.log/drawlist_profile.log instrumentation
// was temporary diagnostic code and has since been fully removed). Overwrites
// fps.log with the latest reading once every 60 frames -- no history kept, no
// unbounded growth across a play session, just a visible number per stage.
// Split into phases so the number is actionable rather than just a score.
//   raster -- measured directly around the actual pixel work (the draw-queue
//             flush, or the immediate rasterizing that replaces it when the
//             queue is bypassed) plus the framebuffer cache maintenance. This
//             is what GPU acceleration is meant to attack, and the number
//             that has to move for any of that work to be worth it.
//   rest   -- everything else in the frame: game logic (input, physics,
//             entity updates) plus FlipScreen's sceDisplayWaitVblankStart.
//             That vblank wait is IDLE time, so if `rest` is large the frame
//             is waiting on the display rather than computing, and making
//             drawing faster cannot raise fps.
// Also reports the achieved CPU/bus clock, to confirm what the hardware
// actually did rather than trusting that scePowerSetClockFrequency took.
static SceUInt64 gu_rasterStartTick = 0;

void GU_MarkRasterStart() { gu_rasterStartTick = sceKernelGetSystemTimeWide(); }
void GU_MarkRasterEnd()
{
    if (gu_rasterStartTick)
        gu_rasterUsecAccum += sceKernelGetSystemTimeWide() - gu_rasterStartTick;
}

static void GU_UpdateFPSCounter()
{
    static int32 frameCount = 0;
    static SceUInt64 lastTick = 0;

    if (++frameCount < 60)
        return;

    SceUInt64 now = sceKernelGetSystemTimeWide();
    if (lastTick != 0) {
        double elapsedSec = (double)(now - lastTick) / 1000000.0;
        double fps        = elapsedSec > 0.0 ? frameCount / elapsedSec : 0.0;
        double rasterMs   = (double)gu_rasterUsecAccum / 1000.0 / frameCount;
        double frameMs    = elapsedSec * 1000.0 / frameCount;
        double vblankMs   = (double)gu_vblankUsecAccum / 1000.0 / frameCount;
        // Split of the non-rendering half of the frame -- see Object.cpp.
        extern SceUInt64 gu_objUpdateUsecAccum, gu_objDrawListUsecAccum;
        double updMs      = (double)gu_objUpdateUsecAccum / 1000.0 / frameCount;
        double dlistMs    = (double)gu_objDrawListUsecAccum / 1000.0 / frameCount;
        double flipMs     = (double)gu_flipUsecAccum / 1000.0 / frameCount;
#if GU_ENABLE_PROFILING
        FILE *f           = fopen("fps.log", "w");
        if (f) {
            fprintf(f, "%.2f fps\n", fps);
            fprintf(f, "frame %.2f ms = compute %.2f + idle(vblank) %.2f\n", frameMs, frameMs - vblankMs, vblankMs);
            fprintf(f, "  of compute: raster %.2f, entity update %.2f, drawlist %.2f, flip %.2f, other %.2f ms\n", rasterMs, updMs, dlistMs, flipMs,
                    frameMs - vblankMs - rasterMs - updMs - dlistMs - flipMs);
            fprintf(f, "  60fps needs compute < 16.67 ms\n");
            fprintf(f, "cpu %d MHz, bus %d MHz\n", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
            fprintf(f, "queue %s, gpu sprites %s\n", GU_BYPASS_DRAW_QUEUE ? "BYPASSED" : "on", GU_AB_FORCE_CPU_ONLY ? "off" : "ON");

            static const char *typeNames[GU_ENTRY_TYPE_COUNT] = { "sprite", "layer",  "fillscreen", "rect",         "rotozoom",
                                                                  "face",   "bfaced", "circle",     "circleoutline" };
            fprintf(f, "\nper-frame cost by draw type:\n");
            for (int32 t = 0; t < GU_ENTRY_TYPE_COUNT; ++t) {
                if (!gu_profCount[t])
                    continue;
                fprintf(f, "  %-14s %6.2f ms  %5.1f draws\n", typeNames[t], (double)gu_profUsec[t] / 1000.0 / frameCount,
                        (double)gu_profCount[t] / frameCount);
            }
            fclose(f);
        }

        // Rolling history, one line per window, appended.
        //
        // fps.log alone only ever holds the LAST window, and this game can
        // only be exited through its own main menu -- so the final window is
        // always the menu, never gameplay. Every profile read so far was
        // therefore menu data, with the menu's animated background (dozens of
        // face/circleoutline draws per frame) mistaken for gameplay cost.
        // This history lets a normal play-and-quit session be read back scene
        // by scene, so gameplay windows can be found directly instead of
        // relying on cutting power mid-frame to catch one.
        //
        // One line per ~3.4s of play: negligible I/O, unlike the per-draw
        // logging tried earlier, which stalled the game to ~1fps.
        static int32 windowIndex = 0;
        if (windowIndex < 2000) {
            FILE *h = fopen("fps_history.log", windowIndex == 0 ? "w" : "a");
            if (h) {
                fprintf(h, "%3d  %5.2f fps  frame %6.2f  cpu %6.2f  idle %6.2f  raster %6.2f  upd %6.2f  dlist %6.2f  flip %6.2f  other %6.2f  |  "
                           "spr %5.2f/%-5.1f  lay %5.2f  fill %5.2f/%-4.1f  face %5.2f/%-5.1f  bface %5.2f/%-5.1f  cout %5.2f/%-5.1f  rect %5.2f  "
                           "circ %5.2f\n",
                        windowIndex, fps, frameMs, frameMs - vblankMs, vblankMs, rasterMs, updMs, dlistMs, flipMs,
                        frameMs - vblankMs - rasterMs - updMs - dlistMs - flipMs, (double)gu_profUsec[GU_ENTRY_SPRITE] / 1000.0 / frameCount,
                        (double)gu_profCount[GU_ENTRY_SPRITE] / frameCount, (double)gu_profUsec[GU_ENTRY_LAYER] / 1000.0 / frameCount,
                        (double)gu_profUsec[GU_ENTRY_FILLSCREEN] / 1000.0 / frameCount,
                        (double)gu_profCount[GU_ENTRY_FILLSCREEN] / frameCount, (double)gu_profUsec[GU_ENTRY_FACE] / 1000.0 / frameCount,
                        (double)gu_profCount[GU_ENTRY_FACE] / frameCount, (double)gu_profUsec[GU_ENTRY_BLENDEDFACE] / 1000.0 / frameCount,
                        (double)gu_profCount[GU_ENTRY_BLENDEDFACE] / frameCount, (double)gu_profUsec[GU_ENTRY_CIRCLEOUTLINE] / 1000.0 / frameCount,
                        (double)gu_profCount[GU_ENTRY_CIRCLEOUTLINE] / frameCount, (double)gu_profUsec[GU_ENTRY_RECT] / 1000.0 / frameCount,
                        (double)gu_profUsec[GU_ENTRY_CIRCLE] / 1000.0 / frameCount);
                fprintf(h, "     queue: peak %4d / %d  drains %d  (drains > 0 means the frame exceeded the queue)\n", gu_queuePeak,
                        GU_DRAW_QUEUE_MAX, gu_queueDrains);
                gu_queuePeak   = 0;
                gu_queueDrains = 0;

                fprintf(h, "     flip breakdown: build %5.2f  enqueue %5.2f  finish %5.2f  sync %5.2f  start %5.2f  |  roto %5.2f\n",
                        (double)gu_flipBuildUsec / 1000.0 / frameCount, (double)gu_flipEnqUsec / 1000.0 / frameCount,
                        (double)gu_flipFinUsec / 1000.0 / frameCount, (double)gu_flipSyncUsec / 1000.0 / frameCount,
                        (double)gu_flipStartUsec / 1000.0 / frameCount, (double)gu_profUsec[GU_ENTRY_ROTOZOOM] / 1000.0 / frameCount);

                // Scene3D pipeline split -- the Special Stage's cost lives
                // inside ProcessObjectDrawLists and none of the draw-type
                // counters above account for it.
                {
                    extern SceUInt64 gu_s3dMeshUsec, gu_s3dSortUsec, gu_s3dDrawUsec;
                    fprintf(h, "     scene3d: mesh(transform) %6.2f  sort %6.2f  draw %6.2f\n", (double)gu_s3dMeshUsec / 1000.0 / frameCount,
                            (double)gu_s3dSortUsec / 1000.0 / frameCount, (double)gu_s3dDrawUsec / 1000.0 / frameCount);
                    gu_s3dMeshUsec = gu_s3dSortUsec = gu_s3dDrawUsec = 0;
                }
                fclose(h);
            }
            windowIndex++;
        }
#else
        (void)rasterMs; (void)vblankMs; (void)updMs; (void)dlistMs; (void)flipMs; (void)fps; (void)frameMs;
#endif
    }
    lastTick           = now;
    frameCount         = 0;
    gu_rasterUsecAccum = 0;
    gu_vblankUsecAccum = 0;
    gu_flipUsecAccum   = 0;
    gu_flipEnqUsec = gu_flipFinUsec = gu_flipSyncUsec = gu_flipStartUsec = gu_flipBuildUsec = 0;
    {
        extern SceUInt64 gu_objUpdateUsecAccum, gu_objDrawListUsecAccum;
        gu_objUpdateUsecAccum   = 0;
        gu_objDrawListUsecAccum = 0;
    }
    memset(gu_profUsec, 0, sizeof(gu_profUsec));
    memset(gu_profCount, 0, sizeof(gu_profCount));
}

void RenderDevice::FlipScreen()
{
    GU_UpdateFPSCounter();

    // Times everything FlipScreen does apart from the vblank wait itself --
    // chiefly sceGuFinish/sceGuSync, which blocks until the GE has finished
    // the present-quad blit out of screen_texture. A ~6.6ms/frame slice of
    // gameplay compute (28% of the budget) currently falls outside every
    // instrumented phase, and this is the largest unmeasured thing left in
    // the main loop.
    const SceUInt64 gu_flipStart = sceKernelGetSystemTimeWide();

    // Rebuild the present quad: blits screen_texture up to the full 480x272
    // display. The sceGuSync that waits on this costs a constant 5.27ms, so
    // it is NOT waited on here -- see the note further down.
    ge_cmd_ptr = ge_cmd;

    GE_CMD(FBP, ((u32)psp_gu_vram_base & 0x00FFFFFF));
    GE_CMD(FBW, (((u32)psp_gu_vram_base & 0xFF000000) >> 8) | PSP_LINE_SIZE);
    GE_CMD(TBP0, ((u32)screen_texture & 0x00FFFFFF));
    // Texture stride is the rasterizer surface's stride, which is now the
    // display's 512 rather than the old 432.
    GE_CMD(TBW0, (((u32)screen_texture & 0xFF000000) >> 8) | MANIA_PITCH);
    GE_CMD(TSIZE0, (8 << 8) | 9);
    GE_CMD(TFLUSH, 0);
    GE_CMD(VTYPE, (1 << 23) | (0 << 11) | (0 << 9) | (3 << 7) | (0 << 5) | (0 << 2) | 3);
    GE_CMD(BASE, 0);
    GE_CMD(IADDR, 0);
    GE_CMD(BASE, ((u32)screen_vertex & 0xFF000000) >> 8);
    GE_CMD(VADDR, ((u32)screen_vertex & 0x00FFFFFF));
    GE_CMD(PRIM, (6 << 16) | 2);
    GE_CMD(FINISH, 0);
    GE_CMD(SIGNAL, 0);
    GE_CMD(NOP, 0);
    GE_CMD(NOP, 0);

    gu_flipBuildUsec += sceKernelGetSystemTimeWide() - gu_flipStart;

    {
        const SceUInt64 t0 = sceKernelGetSystemTimeWide();
        // Both the command list and its vertices live in cached main RAM, so
        // they have to be pushed out of the data cache before the GE is
        // pointed at them.
        sceKernelDcacheWritebackRange(ge_cmd, sizeof(ge_cmd_buffers[0]));

        // Keep the queue id so CopyFrameBuffer can wait on THIS list
        // specifically via sceGeListSync, instead of going through the GU
        // driver's list bookkeeping (sceGuFinish/sceGuSync/sceGuStart).
        // Deferring the GU-driver sync black-screens; with GPU sprite draws
        // disabled nothing actually uses the sceGu display list, so the
        // present quad can be managed entirely through the raw GE queue and
        // the driver left out of the per-frame path altogether.
        gu_presentListId = sceGeListEnQueue(ge_cmd, ge_cmd_ptr, gecbid, NULL);
        gu_flipEnqUsec += sceKernelGetSystemTimeWide() - t0;
    }

    // Time the vblank wait explicitly. Gameplay frame time sits pinned at
    // ~34.2ms even as raster work swings by several ms between windows,
    // which is the signature of being locked to two vblank periods (33.3ms)
    // rather than being compute-bound -- so an unknown part of each frame is
    // spent idle here. Without measuring it, "rest" conflates real game logic
    // with that idle time, and there's no way to tell how much headroom
    // actually exists before optimizing anything.
    {
        const SceUInt64 waitStart = sceKernelGetSystemTimeWide();
        sceDisplayWaitVblankStart();
        gu_vblankThisFrame = sceKernelGetSystemTimeWide() - waitStart;
        gu_vblankUsecAccum += gu_vblankThisFrame;
    }


    // NO sceGuSync here. Waiting for the GE to finish the present blit costs
    // a measured, constant 5.27ms of pure CPU stall -- a quarter of the frame
    // budget spent idle while a frame's worth of game logic and rasterizing
    // waits behind it.
    //
    // Nothing in this function needs the blit finished. The only thing that
    // does is the DMA that overwrites screen_texture, so the wait happens
    // there instead (see CopyFrameBuffer). By then the GE has had all of
    // ProcessObjects plus the entire draw-queue flush to work in parallel, so
    // the blit and the frame's CPU work overlap instead of running back to
    // back.
    //
    // Everything the GE reads while that overlap is in flight -- this display
    // list, the present-quad command list, and its vertices -- is
    // double-buffered and alternated below, so the CPU never rewrites
    // something the GE is still reading.
    // No sceGuFinish/sceGuSync/sceGuStart at all. Init already closed its
    // setup list, and with GPU sprite draws disabled nothing issues sceGu*()
    // draws during a frame -- so the GU driver's per-frame list machinery is
    // pure overhead here, and it's exactly what black-screens when the sync
    // is deferred. The present quad goes through the raw GE queue instead and
    // is waited on by id in CopyFrameBuffer.
    //
    // Alternate the buffers the GE reads while the overlap is in flight (the
    // command list and its vertices), so the CPU never rewrites one the GE
    // is still executing.
    present_buffer_index ^= 1;

    // Excludes the vblank wait, which is accounted separately as idle.
    gu_flipUsecAccum += (sceKernelGetSystemTimeWide() - gu_flipStart) - gu_vblankThisFrame;
    gu_vblankThisFrame = 0;
}

void RenderDevice::Release(bool32 isRefresh)
{
  if (scanlines)
    free(scanlines);

  if (!isRefresh) {
    //gfxExit();
  }
}

void RenderDevice::RefreshWindow()
{

}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8* imagePixels)
{
  // TODO: implement
  return;
}

// TODO: you may have to rewrite parts of the engine elsewhere to hack 
// 3ds-theoraplayer in, just leave like this for now
void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

void RenderDevice::SetupVideoTexture_YUV424(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

bool RenderDevice::ProcessEvents()
{
  return true;
}

// TODO: re-use the frame limiter @JeffRuLz implemented with the CD port
void RenderDevice::InitFPSCap()
{
  return;
}

bool RenderDevice::CheckFPSCap()
{
    return true;
}

void RenderDevice::UpdateFPSCap()
{
  return;
}

// NOTE: shaders likely won't ever be supported by the 3DS port; given
// that the Retro Engine's internal resolution matches that of the 3DS's 
// screen, they probably wouldn't look too great if implemented anyways
void RenderDevice::LoadShader(const char* fileName, bool32 linear)
{
  return;
}

bool RenderDevice::InitShaders()
{
  return true;
}

bool RenderDevice::SetupRendering()
{
  // is anything even really needed here?
  return true;
}

void RenderDevice::InitVertexBuffer()
{
  // TODO: is this needed, since the buffer is copied to the screen in SW?
  return;
}

bool RenderDevice::InitGraphicsAPI()
{
  // TODO: implement, if needed 
  return true;
}

void RenderDevice::GetDisplays()
{
  // TODO: implement, if needed
  return;
}

void RenderDevice::GetWindowSize(int32* width, int32* height) {
  if (width)
    *width = 480;

  if (height)
    *height = 272;
}
