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
#define GE_CMD_TSYNC  0xCC
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
// Texture and CLUT, also taken from libpspgu.a.
#define GE_CMD_TSCALEU  0x48
#define GE_CMD_TSCALEV  0x49
#define GE_CMD_TOFFSETU 0x4A
#define GE_CMD_TOFFSETV 0x4B
#define GE_CMD_CBP      0xB0
#define GE_CMD_CBPH     0xB1
#define GE_CMD_TMODE    0xC2
#define GE_CMD_TPSM     0xC3
#define GE_CMD_CLOAD    0xC4
#define GE_CMD_CMODE    0xC5
#define GE_CMD_TFLT     0xC6
#define GE_CMD_TWRAP    0xC7
#define GE_CMD_TFUNC    0xC9
#define GE_CMD_ATST     0xdb

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
// A coalesced tile run writes 9 header words, 21 per batch (scissor pair,
// GU_EmitTileTextureState's 15, and VTYPE/BASE/VADDR/PRIM) and an 8-word
// footer. At 64 words that overflowed at three batches: the FINISH/END
// terminators landed outside the buffer, so the GE ran off the end of the list
// and executed adjacent memory as commands -- colour noise, then the console
// powering off. Layer banding is what first produced runs that long.
#define GE_TRI_CMD_WORDS 1024
#define GE_TRI_RUN_HEADER 9
#define GE_TRI_RUN_FOOTER 8
#define GE_TRI_PER_BATCH  21
// Longest run that certainly fits, with a word of slack.
#define GU_TILE_RUN_MAX ((GE_TRI_CMD_WORDS - GE_TRI_RUN_HEADER - GE_TRI_RUN_FOOTER - 1) / GE_TRI_PER_BATCH)

static u32 __attribute__((aligned(16))) ge_cmd_buffers[2][64];
#define ge_cmd (ge_cmd_buffers[present_buffer_index])
static u16 *psp_gu_vram_base = (u16 *)(0x44000000);//0x600000

// The display was single-buffered: the present quad rendered straight into the
// buffer the LCD was scanning out, so the panel showed a half-updated frame
// every frame. It reads as tearing along horizontal edges and is worst wherever
// the image moves fastest -- the foreground and ground at the bottom of the
// screen -- while the near-static sky looks clean.
//
// Second buffer sits at the top of VRAM, above the sprite arena.
#define PSP_DISP_BYTES ((size_t)PSP_LINE_SIZE * PSP_SCREEN_HEIGHT * sizeof(u16))
static u16 *gu_dispFront = (u16 *)(0x44000000);
static u16 *gu_dispBack  = (u16 *)(0x44000000 + PSP_DISP_BYTES);
static bool gu_dispPending = false;
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
static u16 *screen_texture = (u16 *)(0x4000000 + 2 * (512 * 272 * 2));
static u16 *current_screen_texture = (u16 *)(0x4000000 + 2 * (512 * 272 * 2));

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
static u16 *gu_3d_scratch = (u16 *)(0x4000000 + 2 * (512 * 272 * 2) + (448 * 240 * 2));
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
    GU_ENTRY_FACEBATCH,
    GU_ENTRY_TILEBATCH,
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

// --- Tile layer atlas (Stage C) -----------------------------------------
//
// Set to 1 to build the tileset atlas and draw it on screen for
// verification. This exists to prove texture upload, CLUT and sampling
// independently of any layer geometry -- the same approach the 3D face path
// was brought up with.
#define GU_TILE_ATLAS_TEST 0
#define GU_FB_DUMP    0
#define GU_VRAM_BENCH 0
#define GU_XFORM_BENCH 0

// Rasterize straight into VRAM instead of main RAM. The GE cannot render into
// main RAM, so every GPU batch previously copied the framebuffer up to VRAM
// and back -- 1.3ms per round trip measured on hardware, against 0.67ms of
// actual GE work. Measured on hardware (GU_VramWriteBench), CPU writes to VRAM
// are FASTER than to main RAM (0.76x blit, 0.88x fill, 0.92x read-modify-
// write), so the round trips go away and the software rasterizer gets quicker
// too. Also returns ~212KB of main RAM, which is scarce here.
#define GU_FB_IN_VRAM 1

#if GU_FB_IN_VRAM
#define GU_FPS_LOG "fps_vram.log"
#else
#define GU_FPS_LOG "fps_mainram.log"
#endif

#if GU_FB_IN_VRAM
#define GU_FB_COPY_UP()   ((void)0)
#define GU_FB_COPY_BACK() ((void)0)
// After the GE has drawn into the framebuffer, the CPU's cached copy of those
// lines is stale. It must be DISCARDED, not written back: the rasterizer
// dirtied those same lines earlier in the frame, and writing them back lays
// stale CPU pixels back over what the GE just produced. That is harmless while
// the GE renders into a separate scratch buffer and a DMA carries the result
// back, but with the framebuffer resident in VRAM they are the same memory. A
// cache line is 64 bytes = 32 pixels, which is the width of the bands it
// produces.
#define GU_FB_GE_DONE()   sceKernelDcacheInvalidateRange(screen_pixels, PRESENT_BUFFER_BYTES)
#else
#define GU_FB_COPY_UP()   sceDmacMemcpy(gu_3d_scratch, screen_pixels, bytes)
#define GU_FB_COPY_BACK() sceDmacMemcpy(screen_pixels, gu_3d_scratch, bytes)
// The DMA above is the authority for screen_pixels here, so the whole-cache
// call that follows it is correct in this configuration.
#define GU_FB_GE_DONE()   sceKernelDcacheWritebackInvalidateAll()
#endif
#define GU_FB_DUMP_AT 600

// Draws one tile through the GE and diffs it against the CPU result. Use this
// instead of inspecting screenshots -- the framebuffer is the ground truth.
#define GU_TILE_SELFTEST 0

// The tileset is TILE_COUNT (1024) tiles of 16x16 8-bit palette indices,
// which is exactly a 512x512 texture at 32x32 tiles. tilesetPixels actually
// holds FOUR copies (one per flip variant), but only FLIP_NONE is uploaded:
// the GE flips by swapping texture coordinates, so the other three are free.
#define GU_ATLAS_DIM       512
#define GU_ATLAS_TILES_ROW (GU_ATLAS_DIM / TILE_SIZE) // 32
// Above this a layer is genuinely warped rather than scrolled, and the
// per-band batches would cost more than the CPU rasterizer.
#define GU_TILE_MAX_BANDS  8
// Diagnostic: hold L to route layers back to the CPU rasterizer for a live A/B.
#define GU_TILE_PAD_TOGGLE 0
static u8 *gu_tile_atlas       = NULL;
static int32 gu_tile_atlas_ok  = 0;
static int32 gu_atlas_scene    = -1; // listPos the atlas was built for
static uint8 gu_tile_dirty[TILE_COUNT];  // tiles the engine has rewritten
static bool gu_atlas_all_dirty = true;
static int32 gu_atlas_changed  = 0;

void RSDK::GU_MarkTilesDirty(int32 first, int32 count)
{
    if (first < 0) { count += first; first = 0; }
    if (first >= TILE_COUNT || count <= 0)
        return;
    if (first + count > TILE_COUNT)
        count = TILE_COUNT - first;
    memset(&gu_tile_dirty[first], 1, (size_t)count);
}

void RSDK::GU_MarkAllTilesDirty() { gu_atlas_all_dirty = true; }

#if GU_TILE_PAD_TOGGLE
#include <pspctrl.h>
// Hold L to route every layer, or R every 3D face, back to the CPU rasterizer,
// so the same spot in the same run can be compared both ways.
static bool GU_PadDisablesTiles()
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
        return (pad.Buttons & PSP_CTRL_LTRIGGER) != 0;
    return false;
}

static bool GU_PadDisablesFaces()
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
        return (pad.Buttons & PSP_CTRL_RTRIGGER) != 0;
    return false;
}
#endif
static int32 gu_atlas_frame    = -1; // frame the hash was last checked on
static int32 gu_frameCounter   = 0;
static void GU_BuildTileAtlas();

// Sparse hash of the tileset. Rebuilding the atlas on scene change alone is
// wrong: Mania loads tile graphics after the scene has started, so the atlas
// was captured from a tileset that was still empty (74 non-zero samples of
// ~37000) and never refreshed, leaving every tile transparent. Tileset loads
// rewrite large regions, so a few hundred samples detect them reliably, and
// this runs once per frame rather than per layer.

// Why layers get refused by GU_TryQueueLayerGPU, indexed by reason. Every
// refusal is silent otherwise, which makes "nothing went to the GE" and
// "everything went to the GE and drew nothing" look identical from outside.
static int32 gu_tile_decline[9] = { 0 };
static int32 gu_faceRejectRange = 0;  // projected outside the GE's usable range
static int32 gu_faceRejectFull  = 0;  // vertex buffer exhausted this frame
static int32 gu_faceRejectInk   = 0;  // ink effect or vert count the GE path cannot express
static FILE *gu_layerLog = NULL;
static int32 gu_layerLogFrame = 0;
// distinct per-scanline palette banks over the clip range, for diagnosis
static int32 GU_BankSpread(int32 y, int32 h, int32 *firstBank, int32 *lastBank)
{
    uint8 seen[256]; int32 n = 0;
    for (int32 i = 0; i < 256; ++i) seen[i] = 0;
    for (int32 i = 0; i < h; ++i) {
        const uint8 b = gfxLineBuffer[y + i];
        if (!seen[b]) { seen[b] = 1; ++n; }
    }
    *firstBank = gfxLineBuffer[y];
    *lastBank  = gfxLineBuffer[y + h - 1];
    return n;
}
// Which palette bank the GE currently holds in its CLUT, reset each frame
// because the present list runs in between and may leave anything loaded.
static int32 gu_clutLoadedBank = -1;
#define GU_DECLINE(n) do { ++gu_tile_decline[(n)];                                        \
    if (gu_layerLog) {                                                                     \
        int32 fb_ = 0, lb_ = 0;                                                            \
        const int32 ns_ = GU_BankSpread(currentScreen->clipBound_Y1,                       \
            currentScreen->clipBound_Y2 - currentScreen->clipBound_Y1, &fb_, &lb_);        \
        fprintf(gu_layerLog, "  CPU  type=%d draw=%d size=%dx%d reason=%d banks=%d(%d..%d)\n", \
            (int)layer->type, (int)layer->drawGroup[0], (int)layer->xsize,                 \
            (int)layer->ysize, (int)(n), (int)ns_, (int)fb_, (int)lb_);                    \
    }                                                                                      \
    return false; } while (0)

// Textured 2D vertex: GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D.
// Component order is fixed by the hardware: texture coords, then position.
// All members are 16-bit so the 10-byte stride is legal as-is.
struct GUTexVertex {
    s16 u, v;
    s16 x, y, z;
};

// Repacks tilesetPixels into the atlas. Tile N lands at column N&31, row N>>5.
//
// The source is tile-major (256 consecutive bytes per tile), so this is 16
// row copies per tile rather than one -- 1024 tiles x 16 rows of 16 bytes.
// Done once per scene, not per frame.
// --- GPU tile layers (Stage C) ------------------------------------------
//
// Set to 1 to route eligible tile layers to the GE. Off until verified.
#define GU_GPU_TILES 1

// One quad per visible tile: 27x16 covers a 424x240 screen with a partial
// tile at each edge, so ~432 quads (864 verts) per layer, a handful of
// layers per frame.
#define GU_TILE_VERT_WANT 4096
static GUTexVertex *gu_tile_verts   = NULL;
static int32 gu_tile_vert_count     = 0;
static int32 gu_tile_vert_max       = 0;
static SceUInt64 gu_tileDmaUsec = 0, gu_tileGeUsec = 0;
static int32 gu_tileBatchCount = 0, gu_tileQuadCount = 0;

struct GUTileBatchEntry {
    int32 firstVert, vertCount, bank;
    // Snapshot, not a pointer: clipBound_* are rewritten on the live
    // ScreenInfo as the frame draws, so by flush time they no longer
    // describe the region this layer was clipped to. Reading them late
    // gave SCISSOR2 a -1 and scissored the whole layer away.
    ScreenInfo screenSnapshot;
};

// Builds the quads for one BASIC layer, or returns false to leave it on the
// CPU path.
//
// BASIC is the tractable case: DrawLayerBasic uses fullPalette[0] for the
// whole layer and a single scroll offset taken from the first scanline, so
// the layer is a plain tile grid. HSCROLL is the same shape (its scanline x
// span measures 0, i.e. all lines share a scroll) but is left alone for now.
// ROTOZOOM genuinely varies per scanline and stays on the CPU.
//
// Deliberately mirrors DrawLayerBasic rather than improving on it: the tile
// index is masked with 0xFFF and flip bits are IGNORED, because the CPU path
// ignores them too -- matching output matters more than correctness here.

// --- GPU face batching (Stage B) ----------------------------------------
//
// Set to 1 to route eligible 3D faces to the GE instead of the CPU
// rasterizer. Off by default until it has been verified on hardware.
#define GU_GPU_FACES 1

// Vertices for one frame's GPU faces, expanded to triangles. Measured on
// hardware, the Special Stage emits ~1760 faces/frame; at 3 verts each that
// is ~5300, and quads expand to two triangles, so this has real headroom.
// Anything beyond it falls back to the CPU rasterizer rather than dropping.
// Allocated on the heap at Init, NOT as a static array. As 147KB of static
// BSS this was enough to make the framebuffer allocation in Init fail on
// hardware, and a failed Init exits the engine straight back to the XMB.
// PPSSPP has a looser memory budget and booted fine, which hid it entirely.
// Init tries progressively smaller sizes and disables the GPU path rather
// than failing if none fit.
static GUFaceVertex *gu_face_verts = NULL;
static int32 gu_face_vert_max      = 0;
static int32 gu_face_vert_count = 0;

// Per-window profiling for the GPU face path.
static SceUInt64 gu_faceDmaUsec = 0, gu_faceGeUsec = 0;
static int32 gu_faceBatchCount = 0, gu_faceTriCount = 0;

// Tile-layer profiling: cost per layer type, and scanline-band counts.
static SceUInt64 gu_layerTypeUsec[4] = { 0, 0, 0, 0 };
static int32 gu_layerBands = 0, gu_layerBandSamples = 0;

// A run of consecutive GPU faces, replayed as one GE draw. Consecutive
// eligible faces extend the open batch rather than each taking a queue slot,
// which is what keeps 1760 faces from overflowing a 2048-entry queue -- and
// is the whole point, since one GE list replaces 1760 software rasterizes.
//
// The batch is closed as soon as any other draw is queued, so draw order is
// preserved exactly: the HUD sprites drawn after the 3D still land on top.
struct GUFaceBatchEntry {
    ScreenInfo screenSnapshot;
    int32 firstVert, vertCount;
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
        GUFaceBatchEntry faceBatch;
        GUTileBatchEntry tileBatch;
        GUCircleEntry circle;
        GUCircleOutlineEntry circleOutline;
    };
};

// 2048 entries at 316 bytes each was 647KB of static BSS -- the single
// largest allocation in the port, and it squeezed the newlib heap the game
// uses for stage and tile-layer data. It only needed to be that big because
// every 3D face took a slot; with faces batched, measured peak is 135.
// Overflow is still handled correctly (GU_DrainQueueIfFull replays in order),
// so this is a safety margin, not a hard limit.
#define GU_DRAW_QUEUE_MAX 512
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
#if GU_GPU_FACES
static void GU_DrawFaceBatch(int32 firstVert, int32 vertCount);
#endif
#if GU_GPU_TILES || GU_TILE_SELFTEST || GU_TILE_ATLAS_TEST
static void GU_DrawTileBatch(int32 firstVert, int32 vertCount, int32 bank);
static void GU_DrawTileBatchRun(int32 firstEntry, int32 entryCount);
#endif

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

#if GU_ENABLE_PROFILING
    // Cost per layer type, and the number of distinct per-scanline scroll
    // values (bands). One band means the whole layer can be drawn as a
    // single grid of tile quads on the GE; 240 bands means it cannot.
    const SceUInt64 layerT0 = sceKernelGetSystemTimeWide();
    {
        static uint32 seenDims[64];
        static int32 seenCount = 0;
        const uint32 key = ((uint32)layer->type << 28) ^ ((uint32)layer->xsize << 14) ^ (uint32)layer->ysize;
        int32 known = 0;
        for (int32 i = 0; i < seenCount; ++i)
            if (seenDims[i] == key) { known = 1; break; }
        if (!known && seenCount < 64) {
            seenDims[seenCount++] = key;
            int32 minX = 0x7FFFFFFF, maxX = -0x7FFFFFFF;
            int32 minY = 0x7FFFFFFF, maxY = -0x7FFFFFFF;
            int32 minDX = 0x7FFFFFFF, maxDX = -0x7FFFFFFF;
            for (int32 cy = currentScreen->clipBound_Y1; cy < currentScreen->clipBound_Y2; ++cy) {
                const ScanlineInfo *s = &scanlines[cy];
                const int32 px = s->position.x >> 16, py = s->position.y >> 16;
                if (px < minX) minX = px;   if (px > maxX) maxX = px;
                if (py < minY) minY = py;   if (py > maxY) maxY = py;
                if (s->deform.x < minDX) minDX = s->deform.x;
                if (s->deform.x > maxDX) maxDX = s->deform.x;
            }
            FILE *lf = fopen("layer_dims.log", "a");
            if (lf) {
                fprintf(lf, "type=%d (0=H 1=V 2=ROTO 3=BASIC)  size=%dx%d tiles (%dx%d px)\n",
                        (int)layer->type, (int)layer->xsize, (int)layer->ysize,
                        (int)layer->xsize * 16, (int)layer->ysize * 16);
                fprintf(lf, "   scanline x span = %d..%d (%d px)   y span = %d..%d (%d px)\n",
                        (int)minX, (int)maxX, (int)(maxX - minX), (int)minY, (int)maxY, (int)(maxY - minY));
                fprintf(lf, "   deform.x span   = %d..%d\n\n", (int)minDX, (int)maxDX);
                fclose(lf);
            }
        }
    }
    if (layer->type == LAYER_HSCROLL) {
        int32 bands = 0;
        int32 lastX = 0x7FFFFFFF, lastY = 0x7FFFFFFF;
        for (int32 cy = currentScreen->clipBound_Y1; cy < currentScreen->clipBound_Y2; ++cy) {
            const ScanlineInfo *s = &scanlines[cy];
            if (s->position.x != lastX || s->position.y != lastY) {
                ++bands;
                lastX = s->position.x;
                lastY = s->position.y;
            }
        }
        gu_layerBands += bands;
        gu_layerBandSamples++;
    }
#endif

    switch (layer->type) {
        case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
        case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
        case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
        case LAYER_BASIC: DrawLayerBasic(layer); break;
        default: break;
    }
#if GU_ENABLE_PROFILING
    if (layer->type >= 0 && layer->type < 4)
        gu_layerTypeUsec[layer->type] += sceKernelGetSystemTimeWide() - layerT0;
#endif
}


static bool GU_TryQueueLayerGPU(TileLayer *layer)
{
#if GU_TILE_PAD_TOGGLE
    if (GU_PadDisablesTiles())
        GU_DECLINE(0);
#endif
    if (layer->type != LAYER_BASIC && layer->type != LAYER_HSCROLL)
        GU_DECLINE(0);
    if (!gu_tile_verts)
        GU_DECLINE(2);
    // Once per frame, before any quads are built, so the atlas always matches
    // the tileset this frame is actually drawing with.
    if (gu_atlas_frame != gu_frameCounter) {
        gu_atlas_frame = gu_frameCounter;
        GU_BuildTileAtlas(); // per-tile, so this is cheap when nothing changed
    }
    if (!gu_tile_atlas_ok)
        GU_DECLINE(1); // atlas not built yet (no tileset loaded)
    if (!layer->xsize || !layer->ysize)
        GU_DECLINE(4);
    if (currentScreen->clipBound_X1 >= currentScreen->clipBound_X2 || currentScreen->clipBound_Y1 >= currentScreen->clipBound_Y2)
        GU_DECLINE(5);

    const int32 clipX1 = currentScreen->clipBound_X1, clipX2 = currentScreen->clipBound_X2;
    const int32 clipY1 = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;

    // HSCROLL is only a grid when every scanline shares one x scroll.
    // Measured on hardware it does (x span 0 across 240 lines) -- y just
    // advances a line at a time, same as BASIC -- but that is a property
    // of the scene, not a guarantee, so check it rather than assume.
    // Deformation, if any scene uses it, shows up here as a varying x.
    //
    // The palette must be uniform too: DrawLayerHScroll reads a bank per
    // scanline from gfxLineBuffer, and the Special Stage genuinely varies
    // it per line for its horizon gradient. One CLUT cannot express that,
    // so those layers stay on the CPU.
    int32 bank = 0;
    if (!GU_PaletteUniform(clipY1, clipY2 - clipY1, &bank))
        GU_DECLINE(7);

    // Scanlines sharing one integer x scroll form a band that is a plain grid,
    // and each band can be drawn as its own batch. Requiring the raw
    // fixed-point x to be identical across the whole layer was too strict: the
    // Special Stage's HSCROLL layers drift by a single pixel over 240 lines, so
    // they were refused and cost 6ms/frame on the CPU despite being two bands
    // wide. Genuinely warped layers (the 512x512 rotozoom spans 1582px) produce
    // far more bands than this and still decline.
    //
    // Banding on the integer position, not the fixed-point one, is exact: the
    // CPU rasterizer addresses tiles in whole pixels too.
    int32 bandStart[GU_TILE_MAX_BANDS], bandEnd[GU_TILE_MAX_BANDS], bandX[GU_TILE_MAX_BANDS];
    int32 bandCount = 0;

    if (layer->type == LAYER_BASIC) {
        // DrawLayerBasic uses one scroll for the whole layer regardless of what
        // the individual scanlines say, so it is always a single band.
        bandStart[0] = clipY1;
        bandEnd[0]   = clipY2;
        bandX[0]     = FROM_FIXED(scanlines[clipY1].position.x);
        bandCount    = 1;
    }
    else {
        int32 cur = FROM_FIXED(scanlines[clipY1].position.x);
        int32 st  = clipY1;
        for (int32 cy = clipY1 + 1; cy < clipY2; ++cy) {
            const int32 xv = FROM_FIXED(scanlines[cy].position.x);
            if (xv == cur)
                continue;
            if (bandCount == GU_TILE_MAX_BANDS)
                GU_DECLINE(6);
            bandStart[bandCount] = st;
            bandEnd[bandCount]   = cy;
            bandX[bandCount]     = cur;
            ++bandCount;
            st  = cy;
            cur = xv;
        }
        if (bandCount == GU_TILE_MAX_BANDS)
            GU_DECLINE(6);
        bandStart[bandCount] = st;
        bandEnd[bandCount]   = clipY2;
        bandX[bandCount]     = cur;
        ++bandCount;
    }

    // The two layer types differ in where a row starts, and getting this
    // wrong shifts the whole layer: DrawLayerBasic begins at
    // clipBound_X1, but DrawLayerHScroll begins at framebuffer offset 0
    // and spans the full pitch.
    const int32 originX = (layer->type == LAYER_BASIC) ? clipX1 : 0;
    const int32 spanX   = (layer->type == LAYER_BASIC) ? (clipX2 - clipX1) : (int32)currentScreen->pitch;

    // GU_FlushDrawQueue resets gu_tile_vert_count to 0. Draining part-way
    // through a layer therefore invalidates the firstVert of bands already
    // emitted: their queue entries keep pointing high into the buffer while the
    // remaining bands refill it from zero, so the GE reads vertices that are
    // being overwritten underneath it. That is wild screen coordinates and
    // texture addresses -- on hardware it showed as colour noise and then a
    // power-off. Reserve the whole layer's entries first, while flushing is
    // still harmless.
    // GU_FlushDrawQueue resets gu_tile_vert_count to 0, so draining part-way
    // through a layer leaves earlier bands' entries pointing at vertices the
    // remaining bands then overwrite. Reserve the whole layer's entries while
    // flushing is still harmless.
    if (gu_draw_queue_count + bandCount > GU_DRAW_QUEUE_MAX)
        GU_FlushDrawQueue();
    if (gu_draw_queue_count + bandCount > GU_DRAW_QUEUE_MAX)
        GU_DECLINE(8); // still no room: leave the whole layer on the CPU

    // Worst case for the whole layer up front, so a batch is never emitted for
    // some bands and dropped for others -- that would draw a partial layer.
    {
        int32 worstVerts = 0;
        for (int32 b = 0; b < bandCount; ++b) {
            const int32 wx   = originX + bandX[b];
            const int32 wy   = FROM_FIXED(scanlines[bandStart[b]].position.y);
            const int32 cols = (spanX + (wx & 0xF) + TILE_SIZE - 1) / TILE_SIZE + 1;
            const int32 rows = ((bandEnd[b] - bandStart[b]) + (wy & 0xF) + TILE_SIZE - 1) / TILE_SIZE + 1;
            worstVerts += cols * rows * 2 + 8; // +8 covers the per-batch alignment
        }
        if (gu_tile_vert_count + worstVerts > gu_tile_vert_max)
            GU_DECLINE(8); // no room -- CPU path rather than a partial layer
    }

    int32 totalUsed = 0;

    for (int32 b = 0; b < bandCount; ++b) {
        const int32 byStart = bandStart[b], byEnd = bandEnd[b];
        const int32 worldX  = originX + bandX[b];
        const int32 worldY  = FROM_FIXED(scanlines[byStart].position.y);
        const int32 sheetX  = worldX & 0xF, sheetY = worldY & 0xF;
        const int32 tx0 = worldX >> 4, ty0 = worldY >> 4;

        const int32 cols = (spanX + sheetX + TILE_SIZE - 1) / TILE_SIZE + 1;
        const int32 rows = ((byEnd - byStart) + sheetY + TILE_SIZE - 1) / TILE_SIZE + 1;

        gu_tile_vert_count = (gu_tile_vert_count + 7) & ~7;
        const int32 firstVert = gu_tile_vert_count;

        for (int32 j = 0; j < rows; ++j) {
            int32 ty = ty0 + j;
            ty %= layer->ysize;
            if (ty < 0)
                ty += layer->ysize;

            const uint16 *row = &layer->layout[ty << layer->widthShift];
            const int32 sy    = byStart - sheetY + j * TILE_SIZE;

            for (int32 i = 0; i < cols; ++i) {
                int32 tx = tx0 + i;
                tx %= layer->xsize;
                if (tx < 0)
                    tx += layer->xsize;

                const uint16 entry = row[tx];
                if (entry == 0xFFFF)
                    continue; // empty tile

                // Index is 10 bits: TILE_COUNT is 0x400. Bits 10-11 are the
                // flip flags (FlipFlags: 1 = X, 2 = Y), which the CPU path
                // resolves via pre-generated variants in tilesetPixels.
                const int32 tile = entry & 0x3FF;
                const int32 flip = (entry >> 10) & 3;
                const int32 au   = (tile % GU_ATLAS_TILES_ROW) * TILE_SIZE;
                const int32 av   = (tile / GU_ATLAS_TILES_ROW) * TILE_SIZE;
                const int32 sx   = originX - sheetX + i * TILE_SIZE;

                GUTexVertex *v = &gu_tile_verts[gu_tile_vert_count];
                gu_tile_vert_count += 2;

                // Flip by swapping texture coordinates rather than packing all
                // four variants: 4096 tiles would need a 1MB atlas, and the GE
                // mirrors for free when the second corner's u/v run backwards.
                s16 u0 = (s16)au, u1 = (s16)(au + TILE_SIZE);
                s16 v0 = (s16)av, v1 = (s16)(av + TILE_SIZE);
                if (flip & 1) { const s16 t = u0; u0 = u1; u1 = t; }
                if (flip & 2) { const s16 t = v0; v0 = v1; v1 = t; }

                v[0].u = u0;                    v[0].v = v0;
                v[0].x = (s16)sx;               v[0].y = (s16)sy;             v[0].z = 0;
                v[1].u = u1;                    v[1].v = v1;
                v[1].x = (s16)(sx + TILE_SIZE); v[1].y = (s16)(sy + TILE_SIZE); v[1].z = 0;
            }
        }

        const int32 used = gu_tile_vert_count - firstVert;
        if (!used)
            continue;
        totalUsed += used;

        // No drain here -- see the note above; capacity was reserved already.
        GUQueueEntry *e        = &gu_draw_queue[gu_draw_queue_count++];
        e->type                = GU_ENTRY_TILEBATCH;
        e->tileBatch.firstVert = firstVert;
        e->tileBatch.vertCount = used;
        e->tileBatch.bank      = bank;
        // Scissor to this band only, so bands cannot paint over each other.
        e->tileBatch.screenSnapshot = *currentScreen;
        e->tileBatch.screenSnapshot.clipBound_Y1 = byStart;
        e->tileBatch.screenSnapshot.clipBound_Y2 = byEnd;
    }

    if (gu_layerLog) {
        int32 fb_ = 0, lb_ = 0;
        const int32 ns_ = GU_BankSpread(clipY1, clipY2 - clipY1, &fb_, &lb_);
        fprintf(gu_layerLog, "  GPU  type=%d draw=%d size=%dx%d bank=%d bands=%d verts=%d pal=%d(%d..%d)\n",
                (int)layer->type, (int)layer->drawGroup[0], (int)layer->xsize,
                (int)layer->ysize, (int)bank, (int)bandCount, (int)totalUsed, (int)ns_, (int)fb_, (int)lb_);
    }
    return true;
}

void RSDK::GU_QueueLayerDraw(TileLayer *layer)
{
#if GU_BYPASS_DRAW_QUEUE
    GU_DrawLayerImmediate(layer);
    return;
#endif

    GU_DrainQueueIfFull();

#if GU_GPU_TILES
    // Eligible layers go to the GE instead of being queued for the CPU
    // rasterizer. Anything it declines -- ROTOZOOM, per-scanline palette
    // banks, no atlas, no room -- falls through to the path below.
    if (GU_TryQueueLayerGPU(layer))
        return;
#endif

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

#if GU_GPU_FACES
    // Route to the GE when the face is a plain opaque triangle or quad.
    //
    // Anything with an ink effect or partial alpha still goes down the CPU
    // path: those need the blend tables, and getting them onto the GE is a
    // separate problem. Eligible faces are appended to the open batch so a
    // whole run of them costs ONE queue slot and ONE GE draw, rather than
    // 1760 slots and 1760 software rasterizes.
    {
        // One-shot: why faces are or are not eligible.
        static int32 eligDone = 0;
        if (!eligDone && gu_profilingEnabled) {
            eligDone = 1;
            FILE *ef = fopen("elig_dbg.log", "w");
            if (ef) {
                fprintf(ef, "first blended face: inkEffect=%d alpha=%d vertCount=%d\n", (int)inkEffect, (int)alpha, (int)vertCount);
                fprintf(ef, "INK_NONE=%d INK_BLEND=%d INK_ALPHA=%d INK_ADD=%d INK_SUB=%d\n", (int)INK_NONE, (int)INK_BLEND, (int)INK_ALPHA, (int)INK_ADD, (int)INK_SUB);
                fclose(ef);
            }
        }
    }
    // NOTE: alpha is deliberately NOT tested. For INK_NONE the CPU rasterizer
    // ignores it and writes opaquely -- the game passes 0 here -- so requiring
    // alpha >= 0xFF rejected every face in the game.
    if (!(inkEffect == INK_NONE && (vertCount == 3 || vertCount == 4)))
        ++gu_faceRejectInk;
    if (inkEffect == INK_NONE && (vertCount == 3 || vertCount == 4)
#if GU_TILE_PAD_TOGGLE
        && !GU_PadDisablesFaces()
#endif
        ) {
        const int32 needed = (vertCount == 3) ? 3 : 6; // quads become two triangles
        // Reject faces whose projected coordinates are out of range.
        //
        // Draw3DScene projects as (x << projectionX) / z, and z is only
        // rejected below 0x100 -- so a vertex close to the camera can come
        // out at tens of thousands of pixels. The CPU rasterizer clips that
        // correctly; packing it into an s16 wraps it, and the GE's 2D
        // coordinate space is narrower still, so the triangle lands
        // somewhere arbitrary. Those faces go back to the CPU path, which
        // clips properly -- it is a handful per frame, not a hot path.
        int32 inRange = 1;
        for (int32 i = 0; i < vertCount; ++i) {
            const int32 px = vertices[i].x >> 16;
            const int32 py = vertices[i].y >> 16;
            if (px < -1024 || px > 2047 || py < -1024 || py > 2047) {
                inRange = 0;
                break;
            }
        }

        if (!inRange)
            ++gu_faceRejectRange;
        else if (gu_face_verts && gu_face_vert_count + needed > gu_face_vert_max)
            ++gu_faceRejectFull;

        if (inRange && gu_face_verts && gu_face_vert_count + needed <= gu_face_vert_max) {
            // Extend the batch if the previous entry is one and is still the
            // most recent -- otherwise any intervening draw would be
            // reordered behind these faces.
            GUQueueEntry *b = NULL;
            if (gu_draw_queue_count > 0 && gu_draw_queue[gu_draw_queue_count - 1].type == GU_ENTRY_FACEBATCH)
                b = &gu_draw_queue[gu_draw_queue_count - 1];
            else {
                b                          = &gu_draw_queue[gu_draw_queue_count++];
                b->type                    = GU_ENTRY_FACEBATCH;
                b->faceBatch.screenSnapshot = *currentScreen;
                b->faceBatch.firstVert     = gu_face_vert_count;
                b->faceBatch.vertCount     = 0;
            }

            // Screen-space already: Draw3DScene projects into 16.16 fixed
            // point, so this is a shift, not a transform. Colours arrive as
            // 0xRRGGBB and the GE wants 0xAABBGGRR -- red in the LOW byte,
            // the same order the framebuffer uses.
            static const int32 quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int32 i = 0; i < needed; ++i) {
                const int32 s     = (vertCount == 3) ? i : quadIdx[i];
                GUFaceVertex *v   = &gu_face_verts[gu_face_vert_count++];
                const uint32 c    = colors[s];
                v->color          = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                v->x              = (s16)(vertices[s].x >> 16);
                v->y              = (s16)(vertices[s].y >> 16);
                v->z              = 0;
                v->pad            = 0;
            }
            b->faceBatch.vertCount += needed;
            return;
        }
        // Vertex buffer full -- fall through to the CPU path rather than
        // dropping the face.
    }
#endif


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


// Replays every queued entry in original order. Called once per frame from
// CopyFrameBuffer(), after the CPU-side writeback. Stage 0: every entry is
// a CPU-fallback replay, so this is equivalent to what used to happen
// inline during ProcessObjectDrawLists/game object draw callbacks, just
// deferred to one place.
void GU_FlushDrawQueue()
{
#if GU_GPU_TILES
    // Re-issuing CLOAD for an already-loaded CLUT within one list corrupts the
    // texture alpha of every batch after the first: RGB still resolves, but the
    // alpha test then rejects every texel and the layer vanishes. Load it once
    // per flush and skip it while the bank is unchanged.
    gu_clutLoadedBank = -1;
#endif
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
#if GU_GPU_FACES
            case GU_ENTRY_FACEBATCH: {
                GUFaceBatchEntry *fb      = &e->faceBatch;
                ScreenInfo snapshotScreen = fb->screenSnapshot;
                currentScreen             = &snapshotScreen;
                GU_DrawFaceBatch(fb->firstVert, fb->vertCount);
                break;
            }
#endif
#if GU_GPU_TILES
            case GU_ENTRY_TILEBATCH: {
                // Consecutive tile batches share one VRAM round trip.
                int32 last = i;
                while (last + 1 < gu_draw_queue_count && gu_draw_queue[last + 1].type == GU_ENTRY_TILEBATCH
                       && (last - i + 1) < GU_TILE_RUN_MAX)
                    ++last; // a longer run would not fit in ge_tri_cmd
                GU_DrawTileBatchRun(i, last - i + 1);
                i = last;
                break;
            }
#endif
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

    gu_face_vert_count   = 0;
    gu_tile_vert_count   = 0;
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
static u32 __attribute__((aligned(16))) ge_tri_cmd[GE_TRI_CMD_WORDS];
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
    GU_FB_COPY_UP();

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
    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();

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
#if GU_GPU_FACES
// Draws one batch of GPU faces: the finished CPU frame goes out to VRAM, the
// GE draws the triangles onto it there, and the result comes back.
//
// The round trip exists because the GE cannot render into main RAM at all
// (see GU_Draw3DTestTriangleRaw for the evidence). It is per batch rather
// than per frame because a batch is closed by any intervening CPU draw, and
// those draws must see the GE's output -- but consecutive faces coalesce, so
// a scene that draws its 3D in one run pays for exactly one round trip.
static void GU_DrawFaceBatch(int32 firstVert, int32 vertCount)
{
    if (vertCount < 3)
        return;

    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);

    const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    sceKernelDcacheWritebackInvalidateAll();
    GU_FB_COPY_UP();

    const SceUInt64 t1 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    const u32 target = (u32)gu_3d_scratch | 0x40000000; // uncached VRAM alias
    GUFaceVertex *verts = &gu_face_verts[firstVert];

    u32 *saved_ptr = ge_cmd_ptr;
    ge_cmd_ptr     = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);

    GE_CMD(TME, 0);
    GE_CMD(ABE, 0);
    GE_CMD(ATE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1);
    GE_CMD(CULLE, 0); // faces arrive in both windings -- Draw3DScene depth-sorts
    GE_CMD(SHADE, 1); // gouraud (0x50 -- 0x1C is GU_LIGHT1, see the note above)

    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, ((MANIA_HEIGHT - 1) << 10) | (MANIA_WIDTH - 1));

    GE_CMD(VTYPE, (1 << 23) | (2 << 7) | (7 << 2));
    GE_CMD(BASE, ((u32)verts & 0xFF000000) >> 8);
    GE_CMD(VADDR, (u32)verts & 0x00FFFFFF);
    GE_CMD(PRIM, (3 << 16) | vertCount);

    // Restore what FlipScreen's present list assumes but never sets.
    GE_CMD(TME, 1);
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);

    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    ge_cmd_ptr = saved_ptr;

    sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
    sceKernelDcacheWritebackRange(verts, sizeof(GUFaceVertex) * vertCount);

    const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
    if (qid >= 0)
        sceGeListSync(qid, 0);

    const SceUInt64 t2 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();

    if (gu_profilingEnabled) {
        const SceUInt64 t3 = sceKernelGetSystemTimeWide();
        gu_faceDmaUsec += (t1 - t0) + (t3 - t2);
        gu_faceGeUsec += t2 - t1;
        gu_faceBatchCount++;
        gu_faceTriCount += vertCount / 3;
    }
}

// Repacks tilesetPixels into the 512x512 atlas, per tile rather than wholesale.
// Green Hill animates tiles (waterfall, water surface), so the tileset contents
// change every frame; repacking all 1024 tiles each time cost ~2.7ms/frame and
// took the scene from 56fps to 47. Hashing one byte per row of each tile is
// ~16k reads and copies only the tiles that actually changed, which is normally
// a handful.
//
// Writes go through the uncached alias so the GE sees them with no cache
// maintenance: a whole-cache flush per frame costs far more than these copies.
static void GU_BuildTileAtlas()
{
    if (!gu_tile_atlas)
        return;

    u8 *const atlasU = (u8 *)((u32)gu_tile_atlas | 0x40000000);
    int32 changed    = 0;

    for (int32 t = 0; t < TILE_COUNT; ++t) {
        if (!gu_atlas_all_dirty && !gu_tile_dirty[t])
            continue;
        gu_tile_dirty[t] = 0;
        ++changed;

        const u8 *src = &tilesetPixels[TILE_DATASIZE * t];

        u8 *dst      = atlasU + ((t / GU_ATLAS_TILES_ROW) * TILE_SIZE) * GU_ATLAS_DIM
                       + (t % GU_ATLAS_TILES_ROW) * TILE_SIZE;
        const u8 *sp = src;
        for (int32 row = 0; row < TILE_SIZE; ++row) {
            memcpy(dst, sp, TILE_SIZE);
            sp += TILE_SIZE;
            dst += GU_ATLAS_DIM;
        }
    }

    gu_atlas_all_dirty = false;
    gu_tile_atlas_ok   = 1;
    gu_atlas_changed   = changed;


    // Did the source tileset and the packed atlas actually contain
    // anything? A uniform on-screen colour means every texel read as
    // the same index, so check both ends of the copy.
    if (changed > 64) {
        int32 srcNonZero = 0, dstNonZero = 0;
        for (int32 i = 0; i < TILE_COUNT * TILE_DATASIZE; i += 7)
            if (tilesetPixels[i]) ++srcNonZero;
        for (int32 i = 0; i < GU_ATLAS_DIM * GU_ATLAS_DIM; i += 7)
            if (gu_tile_atlas[i]) ++dstNonZero;
        FILE *af = fopen("atlas_dbg.log", "a");
        if (af) {
            fprintf(af, "scene=%d atlas=%p  tileset nonzero=%d  atlas nonzero=%d  tiles repacked=%d\n",
                    (int)sceneInfo.listPos, (void *)gu_tile_atlas, (int)srcNonZero, (int)dstNonZero, (int)changed);
            fclose(af);
        }
    }
}

// Emits the GE commands that bind the atlas as an 8-bit CLUT texture.
// Appended to a list already in progress.
static void GU_EmitTileTextureState(int32 bank)
{
    // CLUT for this palette bank, built with the same 5551 conversion the
    // sprite path uses -- index 0 gets alpha 0, which is RSDK's transparent.
    uint16 *pal = fullPalette[bank];
    for (int32 i = 0; i < 256; ++i) {
        const uint16 c = pal[i];
        const uint16 r5 = c & 0x1F, g6 = (c >> 5) & 0x3F, b5 = (c >> 11) & 0x1F;
        gu_clut[i] = r5 | ((g6 >> 1) << 5) | (b5 << 10) | (i == 0 ? 0 : (1 << 15));
    }
    sceKernelDcacheWritebackRange(gu_clut, sizeof(gu_clut));

    // NOT the uncached alias. CBPH is a 4-bit field holding address bits
    // 24-27 (pspsdk: (cbp >> 8) & 0xf0000), so 0x4B... truncates to 0x4 and
    // the GE reads the CLUT from the wrong address -- every texel then
    // resolves to entry 0, which is transparent, and the whole layer
    // disappears. The writeback below is what keeps it coherent.
    const u32 clutAddr  = (u32)gu_clut;
    const u32 atlasAddr = (u32)gu_tile_atlas;         // already a VRAM address

    GE_CMD(TME, 1);
    GE_CMD(TPSM, 5);   // GU_PSM_T8
    GE_CMD(TMODE, 0);  // no mipmaps, not swizzled
    GE_CMD(TBP0, atlasAddr & 0x00FFFFFF);
    GE_CMD(TBW0, ((atlasAddr & 0xFF000000) >> 8) | GU_ATLAS_DIM);
    GE_CMD(TSIZE0, (9 << 8) | 9); // 2^9 x 2^9 = 512x512

    GE_CMD(CBP, clutAddr & 0x00FFFFFF);
    GE_CMD(CBPH, (clutAddr >> 8) & 0x000F0000);
    // psm | (shift << 2) | (mask << 8) | (start << 16). The mask is NOT
    // optional: passing 0 ANDs every index to zero, so the whole texture
    // samples CLUT entry 0 and comes out a flat colour.
    GE_CMD(CMODE, 1 | (0 << 2) | (0xFF << 8) | (0 << 16)); // 5551, mask 0xFF
    GE_CMD(CLOAD, 256 / 8);     // blocks of 8 entries

    // NEAREST, never linear: bilinear on an indexed texture blends palette
    // indices before the lookup, which is meaningless -- that was confirmed
    // the first time GPU sprites were attempted.
    GE_CMD(TFLT, 0);
    GE_CMD(TWRAP, 0);           // clamp both axes
    GE_CMD(TFUNC, 3 | (1 << 8)); // GU_TFX_REPLACE, RGBA -- these vertices have
                                 // no colour component, so MODULATE would
                                 // multiply the texture by the current
                                 // primitive colour and come out black.
    GE_CMD(TFLUSH, 0);
    GE_CMD(TSYNC, 0);
}

#if GU_GPU_TILES
static GUTexVertex __attribute__((aligned(16))) gu_atlas_test_verts[2];

// Draws the atlas 1:1 at the screen origin. If the tileset appears as a grid
// of tiles in the right colours, then texture upload, CLUT and sampling are
// all correct -- before any layer geometry is involved.
static void GU_DrawTileAtlasTest()
{
    if (!gu_tile_atlas_ok)
        return;

    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);

    int32 bank = 0;
    GU_PaletteUniform(0, MANIA_HEIGHT, &bank);

    gu_atlas_test_verts[0].u = 0;   gu_atlas_test_verts[0].v = 0;
    gu_atlas_test_verts[0].x = 0;   gu_atlas_test_verts[0].y = 0;   gu_atlas_test_verts[0].z = 0;
    gu_atlas_test_verts[1].u = MANIA_WIDTH; gu_atlas_test_verts[1].v = MANIA_HEIGHT;
    gu_atlas_test_verts[1].x = MANIA_WIDTH; gu_atlas_test_verts[1].y = MANIA_HEIGHT; gu_atlas_test_verts[1].z = 0;

    sceKernelDcacheWritebackInvalidateAll();
    GU_FB_COPY_UP();

    const u32 target = (u32)gu_3d_scratch | 0x40000000;

    u32 *saved_ptr = ge_cmd_ptr;
    ge_cmd_ptr     = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);

    GE_CMD(ABE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1);
    GE_CMD(CULLE, 0);
    GE_CMD(SHADE, 0); // flat -- colour comes from the texture

    // Index 0 is transparent; the CLUT gives it alpha 0, so alpha-test it out.
    // (alpha test deliberately not enabled in the verification draw)

    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, ((MANIA_HEIGHT - 1) << 10) | (MANIA_WIDTH - 1));


    GU_EmitTileTextureState(bank);

    // Texture coords in texels (through mode), position 16-bit, 2D.
    GE_CMD(VTYPE, (1 << 23) | (2 << 7) | 2);
    GE_CMD(BASE, ((u32)gu_atlas_test_verts & 0xFF000000) >> 8);
    GE_CMD(VADDR, (u32)gu_atlas_test_verts & 0x00FFFFFF);
    GE_CMD(PRIM, (6 << 16) | 2); // GU_SPRITES, 2 verts

    // Restore what the present quad assumes.
    GE_CMD(ATE, 0);
    GE_CMD(TFLT, 1); // present quad uses GU_LINEAR
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);

    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    ge_cmd_ptr = saved_ptr;

    sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
    sceKernelDcacheWritebackRange(gu_atlas_test_verts, sizeof(gu_atlas_test_verts));

    const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
    if (qid >= 0)
        sceGeListSync(qid, 0);

    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();
}
#endif

#if GU_GPU_TILES
// Draws one batch of tile quads. Same VRAM round trip as the face batch: the
// GE cannot render into main RAM, so the frame goes out, gets drawn on, and
// comes back.

// Draws a run of consecutive queued tile batches through a single VRAM round
// trip. The round trip is the entire cost of this path -- measured at GHZ1 it
// is 3.83ms of DMA against 0.08ms of actual GE work -- so doing one per batch
// made the GPU path slower than the CPU rasterizer it replaced. Batch state
// (scissor, palette, texture) is per-batch inside the one list.
static void GU_DrawTileBatchRun(int32 firstEntry, int32 entryCount)
{
    if (entryCount <= 0 || !gu_tile_atlas_ok)
        return;

    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);

    const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    sceKernelDcacheWritebackInvalidateAll();
    GU_FB_COPY_UP();

    const SceUInt64 t1 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    const u32 target = (u32)gu_3d_scratch | 0x40000000;

    u32 *saved_ptr = ge_cmd_ptr;
    ge_cmd_ptr     = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);

    GE_CMD(ABE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1);
    GE_CMD(CULLE, 0);
    GE_CMD(SHADE, 0);
    GE_CMD(ATE, 1);
    GE_CMD(ATST, (GU_GREATER) | (0 << 8) | (0xFF << 16)); // index 0 is transparent

    int32 drawn = 0;
    for (int32 k = 0; k < entryCount; ++k) {
        const GUTileBatchEntry *tb = &gu_draw_queue[firstEntry + k].tileBatch;
        if (tb->vertCount < 2)
            continue;

        // Never let a batch push the FINISH/END terminators out of the buffer.
        if ((ge_cmd_ptr - ge_tri_cmd) > (GE_TRI_CMD_WORDS - GE_TRI_PER_BATCH - GE_TRI_RUN_FOOTER - 2))
            break;

        const GUTexVertex *verts = &gu_tile_verts[tb->firstVert];

        GE_CMD(SCISSOR1, (tb->screenSnapshot.clipBound_Y1 << 10) | tb->screenSnapshot.clipBound_X1);
        GE_CMD(SCISSOR2, ((tb->screenSnapshot.clipBound_Y2 - 1) << 10) | (tb->screenSnapshot.clipBound_X2 - 1));

        GU_EmitTileTextureState(tb->bank);

        GE_CMD(VTYPE, (1 << 23) | (2 << 7) | 2);
        GE_CMD(BASE, ((u32)verts & 0xFF000000) >> 8);
        GE_CMD(VADDR, (u32)verts & 0x00FFFFFF);
        GE_CMD(PRIM, (6 << 16) | tb->vertCount);

        sceKernelDcacheWritebackRange((void *)verts, sizeof(GUTexVertex) * tb->vertCount);
        ++drawn;

        if (gu_profilingEnabled) {
            gu_tileBatchCount++;
            gu_tileQuadCount += tb->vertCount / 2;
        }
    }

    // Put back what FlipScreen's present list assumes but never sets itself.
    GE_CMD(ATE, 0);
    GE_CMD(TFLT, 1);
    GE_CMD(TFUNC, 3 | (1 << 8));
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);

    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    ge_cmd_ptr = saved_ptr;

    if (drawn) {
        sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
        const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
        if (qid >= 0)
            sceGeListSync(qid, 0);
    }

    const SceUInt64 t2 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();

    if (gu_profilingEnabled) {
        const SceUInt64 t3 = sceKernelGetSystemTimeWide();
        gu_tileDmaUsec += (t1 - t0) + (t3 - t2);
        gu_tileGeUsec += t2 - t1;
    }
}

static void GU_DrawTileBatch(int32 firstVert, int32 vertCount, int32 bank)
{
    if (vertCount < 2 || !gu_tile_atlas_ok)
        return;

    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);

    const SceUInt64 t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    sceKernelDcacheWritebackInvalidateAll();
    GU_FB_COPY_UP();

    const SceUInt64 t1 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    const u32 target   = (u32)gu_3d_scratch | 0x40000000;
    GUTexVertex *verts = &gu_tile_verts[firstVert];

    u32 *saved_ptr = ge_cmd_ptr;
    ge_cmd_ptr     = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);

    GE_CMD(ABE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1);
    GE_CMD(CULLE, 0);
    GE_CMD(SHADE, 0); // flat: colour comes entirely from the texture

    // Tile index 0 is RSDK's transparent pixel and the CLUT gives it alpha 0,
    // so alpha-test it away. Without this the empty parts of every tile would
    // paint over whatever is behind the layer.
    GE_CMD(ATE, 1); // DIAGNOSTIC: test enabled but comparison always passes
    GE_CMD(ATST, (GU_GREATER) | (0 << 8) | (0xFF << 16)); // pass if alpha > 0

    GE_CMD(SCISSOR1, (currentScreen->clipBound_Y1 << 10) | currentScreen->clipBound_X1);
    GE_CMD(SCISSOR2, ((currentScreen->clipBound_Y2 - 1) << 10) | (currentScreen->clipBound_X2 - 1));

    GU_EmitTileTextureState(bank);

    GE_CMD(VTYPE, (1 << 23) | (2 << 7) | 2); // 2D, 16-bit pos, 16-bit texcoords
    GE_CMD(BASE, ((u32)verts & 0xFF000000) >> 8);
    GE_CMD(VADDR, (u32)verts & 0x00FFFFFF);
    GE_CMD(PRIM, (6 << 16) | vertCount); // GU_SPRITES

    // Put back what FlipScreen's present list assumes but never sets itself.
    GE_CMD(ATE, 0);
    GE_CMD(TFLT, 1); // present quad samples with GU_LINEAR
    GE_CMD(TFUNC, 3 | (1 << 8));
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);

    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    ge_cmd_ptr = saved_ptr;

    sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
    sceKernelDcacheWritebackRange(verts, sizeof(GUTexVertex) * vertCount);

    const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
    if (qid >= 0)
        sceGeListSync(qid, 0);

    const SceUInt64 t2 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();

    if (gu_profilingEnabled) {
        const SceUInt64 t3 = sceKernelGetSystemTimeWide();
        gu_tileDmaUsec += (t1 - t0) + (t3 - t2);
        gu_tileGeUsec += t2 - t1;
        gu_tileBatchCount++;
        gu_tileQuadCount += vertCount / 2;
    }
}

#endif
#if GU_TILE_SELFTEST
static GUTexVertex __attribute__((aligned(16))) gu_tile_test_verts[6];
static GUFaceVertex __attribute__((aligned(16))) gu_ctrl_verts[2];

// Draws ONE tile through the GE and diffs the result against what the CPU
// rasterizer would have produced for the same tile.
//
// This exists because the layer output is visibly wrong and guessing at it
// from screenshots has already cost two wrong fixes. The framebuffer is the
// ground truth: compute the expected 16x16 block from tilesetPixels +
// fullPalette, draw the same tile with the GE, read it back, and report
// exactly how they differ. The failure pattern identifies the cause:
//
//   every pixel wrong, same way   -> CLUT / palette
//   pixels from a different tile  -> texture coords or atlas layout
//   block offset by N             -> vertex positions
//   correct but striped           -> sampling / filter / stride
static void GU_TileQuadSelfTest()
{
    static int32 done = 0;
    // Deliberately does NOT require gu_tile_verts: that buffer belongs to the
    // layer path, which is switched off while this diagnostic runs.
    if (done || !gu_tile_atlas_ok)
        return;

    // Pick a tile with plenty of non-zero pixels so the comparison is
    // meaningful rather than mostly-transparent.
    int32 tile = -1;
    for (int32 t = 1; t < TILE_COUNT && tile < 0; ++t) {
        int32 nz = 0;
        for (int32 i = 0; i < TILE_DATASIZE; ++i)
            if (tilesetPixels[TILE_DATASIZE * t + i])
                ++nz;
        if (nz > 120)
            tile = t;
    }
    if (tile < 0)
        return;
    done = 1;

    const u32 pitch    = screens[0].pitch;
    const size_t bytes = (size_t)MANIA_HEIGHT * pitch * sizeof(u16);
    const int32 px = 64, py = 64;
    const int32 bank = 0;

    // Snapshot the destination block before the GE touches it.
    static u16 before[TILE_SIZE][TILE_SIZE];
    for (int32 r = 0; r < TILE_SIZE; ++r)
        for (int32 c = 0; c < TILE_SIZE; ++c)
            before[r][c] = screen_pixels[(py + r) * pitch + px + c];

    const int32 au = (tile % GU_ATLAS_TILES_ROW) * TILE_SIZE;
    const int32 av = (tile / GU_ATLAS_TILES_ROW) * TILE_SIZE;

    // GU_SPRITES with 16-bit vertices does not rasterise here -- an
    // untextured colour sprite in the same list did not draw either,
    // while GU_TRIANGLES with the identical vertex format does (the 3D
    // face path). So build two triangles per tile instead.
    const s16 u0 = (s16)au, v0 = (s16)av;
    const s16 u1 = (s16)(au + TILE_SIZE), v1 = (s16)(av + TILE_SIZE);
    const s16 x0 = (s16)px, y0 = (s16)py;
    const s16 x1 = (s16)(px + TILE_SIZE), y1 = (s16)(py + TILE_SIZE);
    const s16 quad[6][5] = {
        { u0, v0, x0, y0, 0 }, { u1, v0, x1, y0, 0 }, { u0, v1, x0, y1, 0 },
        { u1, v0, x1, y0, 0 }, { u1, v1, x1, y1, 0 }, { u0, v1, x0, y1, 0 },
    };
    for (int32 k = 0; k < 6; ++k) {
        gu_tile_test_verts[k].u = quad[k][0]; gu_tile_test_verts[k].v = quad[k][1];
        gu_tile_test_verts[k].x = quad[k][2]; gu_tile_test_verts[k].y = quad[k][3];
        gu_tile_test_verts[k].z = quad[k][4];
    }

    // Control quad at x=100: opaque red, no texture.
    gu_ctrl_verts[0].color = 0xFF0000FF; gu_ctrl_verts[0].x = 100; gu_ctrl_verts[0].y = (s16)py; gu_ctrl_verts[0].z = 0; gu_ctrl_verts[0].pad = 0;
    gu_ctrl_verts[1].color = 0xFF0000FF; gu_ctrl_verts[1].x = 116; gu_ctrl_verts[1].y = (s16)(py + TILE_SIZE); gu_ctrl_verts[1].z = 0; gu_ctrl_verts[1].pad = 0;

    sceKernelDcacheWritebackInvalidateAll();
    GU_FB_COPY_UP();

    const u32 target = (u32)gu_3d_scratch | 0x40000000;
    u32 *saved_ptr   = ge_cmd_ptr;
    ge_cmd_ptr       = ge_tri_cmd;

    GE_CMD(FBP, target & 0x00FFFFFF);
    GE_CMD(FBW, ((target & 0xFF000000) >> 8) | pitch);
    GE_CMD(ABE, 0);
    GE_CMD(ZTE, 0);
    GE_CMD(ZMSK, 1);
    GE_CMD(CULLE, 0);
    GE_CMD(SHADE, 0);
    GE_CMD(ATE, 1); // DIAGNOSTIC: test enabled but comparison always passes
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, ((MANIA_HEIGHT - 1) << 10) | (MANIA_WIDTH - 1));



    GE_CMD(ATE, 0);
    GE_CMD(TFLT, 1);
    GE_CMD(TFUNC, 3 | (1 << 8));
    GE_CMD(SCISSOR1, 0);
    GE_CMD(SCISSOR2, (PSP_SCREEN_HEIGHT << 10) | PSP_SCREEN_WIDTH);
    GE_CMD(TFLUSH, 0);
    GE_CMD(FINISH, 0);
    GE_CMD(END, 0);

    ge_cmd_ptr = saved_ptr;
    sceKernelDcacheWritebackRange(ge_tri_cmd, sizeof(ge_tri_cmd));
    sceKernelDcacheWritebackRange(gu_tile_test_verts, sizeof(gu_tile_test_verts));
    sceKernelDcacheWritebackRange(gu_ctrl_verts, sizeof(gu_ctrl_verts));

    const int qid = sceGeListEnQueue(ge_tri_cmd, NULL, gecbid, NULL);
    if (qid >= 0)
        sceGeListSync(qid, 0);

    GU_FB_COPY_BACK();
    GU_FB_GE_DONE();

    FILE *f = fopen("tilequad_dbg.log", "w");
    if (!f)
        return;

    const uint16 *pal = fullPalette[bank];
    fprintf(f, "tile=%d  atlas uv=(%d,%d)  screen=(%d,%d)  pitch=%d\n", (int)tile, (int)au, (int)av, (int)px, (int)py, (int)pitch);
    fprintf(f, "clut addr=%08X  clut[%02X]=%04X  clut[0]=%04X  clut[1]=%04X\n",
            (unsigned)(u32)gu_clut, (unsigned)tilesetPixels[TILE_DATASIZE * tile],
            (unsigned)gu_clut[tilesetPixels[TILE_DATASIZE * tile]], (unsigned)gu_clut[0], (unsigned)gu_clut[1]);
    fprintf(f, "pal[%02X]=%04X  before(0,0)=%04X\n",
            (unsigned)tilesetPixels[TILE_DATASIZE * tile],
            (unsigned)fullPalette[bank][tilesetPixels[TILE_DATASIZE * tile]], (unsigned)before[0][0]);
    fprintf(f, "CONTROL untextured sprite at (100,%d): got=%04X (expect non-zero if GU_SPRITES works)\n\n",
            (int)py, (unsigned)screen_pixels[(py + 4) * pitch + 104]);
    fprintf(f, "atlas[0..7] = ");
    for (int32 c = 0; c < 8; ++c)
        fprintf(f, "%02X ", gu_tile_atlas[av * GU_ATLAS_DIM + au + c]);
    fprintf(f, "\ntile [0..7] = ");
    for (int32 c = 0; c < 8; ++c)
        fprintf(f, "%02X ", tilesetPixels[TILE_DATASIZE * tile + c]);
    fprintf(f, "\n\n");

    int32 bad = 0, shown = 0;
    for (int32 r = 0; r < TILE_SIZE; ++r) {
        for (int32 c = 0; c < TILE_SIZE; ++c) {
            const uint8 idx  = tilesetPixels[TILE_DATASIZE * tile + r * TILE_SIZE + c];
            const uint16 exp = idx ? pal[idx] : before[r][c]; // index 0 is transparent
            const uint16 got = screen_pixels[(py + r) * pitch + px + c];
            if (exp != got) {
                ++bad;
                if (shown < 12) {
                    // before[] included: got == before means the GE never
                    // touched the pixel, which is a different fault from
                    // drawing the wrong colour.
                    fprintf(f, "  (%2d,%2d) idx=%02X  expected=%04X  got=%04X  before=%04X\n", (int)c, (int)r,
                            (unsigned)idx, (unsigned)exp, (unsigned)got, (unsigned)before[r][c]);
                    ++shown;
                }
            }
        }
    }
    fprintf(f, "\nmismatched pixels: %d of %d\n", (int)bad, (int)(TILE_SIZE * TILE_SIZE));
    fclose(f);
}
#endif
#if GU_GPU_TILES
#endif
#endif

#define get_screen_pixels()                                                 \
  screen_pixels                                                             \

#define get_screen_pitch()                                                  \
  screen_pitch                                                              \

#if GU_VRAM_BENCH
// Whether the framebuffer can live in VRAM full-time depends entirely on how
// fast the CPU can rasterize into it -- the GE renders there for free, but the
// software rasterizer still writes most pixels. Measures the three access
// shapes the rasterizer actually produces, against main RAM and VRAM, rather
// than assuming VRAM is "about half speed".
static void GU_VramWriteBench()
{
    const int32 pitch = (int32)screens[0].pitch;
    const int32 w = MANIA_WIDTH, h = MANIA_HEIGHT;
    const int32 reps = 8;

    u16 *bufs[2]        = { screen_pixels, gu_3d_scratch };
    const char *names[2] = { "main RAM", "VRAM    " };
    SceUInt64 seq[2], scat[2], rmw[2];

    for (int32 b = 0; b < 2; ++b) {
        volatile u16 *p = bufs[b];
        SceUInt64 t;

        // 1. sequential span fill -- what layers and fillscreen do
        t = sceKernelGetSystemTimeWide();
        for (int32 r = 0; r < reps; ++r)
            for (int32 y = 0; y < h; ++y) {
                volatile u16 *row = p + y * pitch;
                for (int32 x = 0; x < w; ++x)
                    row[x] = (u16)(x + r);
            }
        sceKernelDcacheWritebackInvalidateAll();
        seq[b] = sceKernelGetSystemTimeWide() - t;

        // 2. scattered 16x16 blocks -- what sprite blitting does
        t = sceKernelGetSystemTimeWide();
        for (int32 r = 0; r < reps; ++r)
            for (int32 n = 0; n < 400; ++n) {
                const int32 sx = (n * 37) % (w - 16);
                const int32 sy = (n * 53) % (h - 16);
                for (int32 y = 0; y < 16; ++y) {
                    volatile u16 *row = p + (sy + y) * pitch + sx;
                    for (int32 x = 0; x < 16; ++x)
                        row[x] = (u16)(x + y + r);
                }
            }
        sceKernelDcacheWritebackInvalidateAll();
        scat[b] = sceKernelGetSystemTimeWide() - t;

        // 3. read-modify-write -- what the ink/alpha blend paths do
        t = sceKernelGetSystemTimeWide();
        for (int32 r = 0; r < reps; ++r)
            for (int32 y = 0; y < h; ++y) {
                volatile u16 *row = p + y * pitch;
                for (int32 x = 0; x < w; ++x)
                    row[x] = (u16)((row[x] >> 1) & 0x7BEF);
            }
        sceKernelDcacheWritebackInvalidateAll();
        rmw[b] = sceKernelGetSystemTimeWide() - t;
    }

    FILE *bf = fopen("vram_bench.log", "w");
    if (bf) {
        fprintf(bf, "CPU write cost per full-screen pass (%dx%d, pitch %d), usec\n", (int)w, (int)h, (int)pitch);
        fprintf(bf, "target        seq-fill   sprite-blit   read-mod-write\n");
        for (int32 b = 0; b < 2; ++b)
            fprintf(bf, "%s    %8d   %11d   %14d\n", names[b],
                    (int)(seq[b] / reps), (int)(scat[b] / reps), (int)(rmw[b] / reps));
        fprintf(bf, "\nratio VRAM/main: seq %.2fx  blit %.2fx  rmw %.2fx\n",
                (double)seq[1] / (double)seq[0],
                (double)scat[1] / (double)scat[0],
                (double)rmw[1] / (double)rmw[0]);
        fprintf(bf, "main=%p vram=%p\n", (void *)bufs[0], (void *)bufs[1]);
        fclose(bf);
    }
}
#endif

#if GU_XFORM_BENCH
// Ground truth for the Scene3D vertex cost. In game the transform works out at
// ~4us per vertex, which is roughly fifteen times what nine multiplies and nine
// adds should cost, and the projection phase comes out the same. This runs the
// identical arithmetic over a scratch buffer to separate the maths from
// whatever the in-game version is really waiting on.
//
// Three variants:
//   packed    - 12-byte source, 12-byte destination, sequential
//   scene     - 40-byte destination, matching Scene3DVertex
//   indexed   - 40-byte destination read through an index array, as the real
//               loop does (mdl->vertices[indices[i]])
struct BenchModelVert { int32 x, y, z; };
struct BenchSceneVert { int32 x, y, z, nx, ny, nz, tx, ty; uint32 color; };

static void GU_XformBench()
{
    const int32 N = 4096;
    BenchModelVert *src = (BenchModelVert *)malloc(sizeof(BenchModelVert) * N);
    BenchSceneVert *dst = (BenchSceneVert *)malloc(sizeof(BenchSceneVert) * N);
    int32 *idx          = (int32 *)malloc(sizeof(int32) * N);
    BenchModelVert *dstPacked = (BenchModelVert *)malloc(sizeof(BenchModelVert) * N);
    if (!src || !dst || !idx || !dstPacked)
        return;

    for (int32 i = 0; i < N; ++i) {
        src[i].x = i * 37;  src[i].y = i * 11;  src[i].z = i * 53;
        idx[i]   = (i * 2654435761u) % N;   // scattered, like a real index list
    }

    const int32 m00 = 256, m01 = 0, m02 = 0, m03 = 100;
    const int32 m10 = 0, m11 = 256, m12 = 0, m13 = 200;
    const int32 m20 = 0, m21 = 0, m22 = 256, m23 = 300;

    SceUInt64 t0, tPacked, tScene, tIndexed;
    const int32 reps = 8;

    t0 = sceKernelGetSystemTimeWide();
    for (int32 r = 0; r < reps; ++r)
        for (int32 i = 0; i < N; ++i) {
            dstPacked[i].x = m03 + (m00 * src[i].x >> 8) + (src[i].y * m01 >> 8) + (src[i].z * m02 >> 8);
            dstPacked[i].y = m13 + (src[i].z * m12 >> 8) + (m10 * src[i].x >> 8) + (src[i].y * m11 >> 8);
            dstPacked[i].z = m23 + (m22 * src[i].z >> 8) + (src[i].y * m21 >> 8) + (src[i].x * m20 >> 8);
        }
    tPacked = sceKernelGetSystemTimeWide() - t0;

    t0 = sceKernelGetSystemTimeWide();
    for (int32 r = 0; r < reps; ++r)
        for (int32 i = 0; i < N; ++i) {
            dst[i].x = m03 + (m00 * src[i].x >> 8) + (src[i].y * m01 >> 8) + (src[i].z * m02 >> 8);
            dst[i].y = m13 + (src[i].z * m12 >> 8) + (m10 * src[i].x >> 8) + (src[i].y * m11 >> 8);
            dst[i].z = m23 + (m22 * src[i].z >> 8) + (src[i].y * m21 >> 8) + (src[i].x * m20 >> 8);
        }
    tScene = sceKernelGetSystemTimeWide() - t0;

    t0 = sceKernelGetSystemTimeWide();
    for (int32 r = 0; r < reps; ++r)
        for (int32 i = 0; i < N; ++i) {
            const BenchModelVert *sv = &src[idx[i]];
            dst[i].x = m03 + (m00 * sv->x >> 8) + (sv->y * m01 >> 8) + (sv->z * m02 >> 8);
            dst[i].y = m13 + (sv->z * m12 >> 8) + (m10 * sv->x >> 8) + (sv->y * m11 >> 8);
            dst[i].z = m23 + (m22 * sv->z >> 8) + (sv->y * m21 >> 8) + (sv->x * m20 >> 8);
        }
    tIndexed = sceKernelGetSystemTimeWide() - t0;

    FILE *bf = fopen("xform_bench.log", "w");
    if (bf) {
        const double per = 1000.0 / (double)(N * reps); // usec -> nsec per vertex
        fprintf(bf, "transform cost per vertex, %d verts x %d reps\n\n", (int)N, (int)reps);
        fprintf(bf, "  packed  12B dst, sequential : %7.1f ns\n", (double)tPacked * per);
        fprintf(bf, "  scene   40B dst, sequential : %7.1f ns\n", (double)tScene * per);
        fprintf(bf, "  scene   40B dst, indexed src: %7.1f ns\n", (double)tIndexed * per);
        fprintf(bf, "\nin game the transform measures ~4100 ns/vertex\n");
        fprintf(bf, "cpu %d MHz\n", scePowerGetCpuClockFrequencyInt());
        fclose(bf);
    }

    free(src); free(dst); free(idx); free(dstPacked);
}
#endif

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
#if GU_FB_IN_VRAM
  // Same block the GE was already rendering into as scratch; now it is the
  // framebuffer itself, so there is nothing to copy. Cache-line alignment
  // still matters for the same reason described above, and a VRAM base is
  // 64-byte aligned by construction.
  screen_pixels = gu_3d_scratch;
#else
  screen_pixels = (u16 *)memalign(64, PRESENT_ALLOC_BYTES);
  if (!screen_pixels)
    return false;
#endif

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
      gu_tex_arena = (u8 *)gu_3d_scratch + ((PRESENT_ALLOC_BYTES + 255) & ~(size_t)255);

      // Tile atlas: 512x512 8-bit = 256KB off the front of the arena,
      // which is otherwise unused while GPU sprite drawing is parked.
      gu_tile_atlas = gu_tex_arena;
      gu_tex_arena += (size_t)GU_ATLAS_DIM * GU_ATLAS_DIM;

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

#if GU_GPU_FACES
  // Heap-allocated last, so the framebuffer allocation above always wins
  // if memory is tight. Falls back through smaller sizes and finally to
  // the CPU rasterizer, rather than failing Init -- a failed Init drops
  // straight back to the XMB with no diagnostic at all.
  {
      // Sized against measured need, not headroom: the Special Stage emits
      // ~1760 faces/frame = ~5300 verts on hardware. 6144 covers that at 73KB.
      // Only ~764KB is free after init, so a bigger buffer starves later
      // allocations -- 147KB here was enough to fail Init outright.
      static const int32 wanted[] = { 16384, 12288, 8192, 6144, 4096, 3072, 2048 };
      // (tile quad buffer allocated just below)
      for (uint32 i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
          gu_face_verts = (GUFaceVertex *)memalign(16, sizeof(GUFaceVertex) * wanted[i]);
          if (gu_face_verts) {
              gu_face_vert_max = wanted[i];
              break;
          }
      }

#if GU_GPU_TILES
      {
          static const int32 tileWanted[] = { GU_TILE_VERT_WANT, 3072, 2048 };
          for (uint32 i = 0; i < sizeof(tileWanted) / sizeof(tileWanted[0]); ++i) {
              gu_tile_verts = (GUTexVertex *)memalign(16, sizeof(GUTexVertex) * tileWanted[i]);
              if (gu_tile_verts) {
                  gu_tile_vert_max = tileWanted[i];
                  break;
              }
          }
      }
#endif
      printf("RSDKv5 PSP: gpu face verts = %d (%d bytes), free mem = %d\n",
             (int)gu_face_vert_max, (int)(gu_face_vert_max * (int)sizeof(GUFaceVertex)),
             (int)sceKernelTotalFreeMemSize());

      // Memory report. Gated on profiling -- a shipping build has no
      // business writing to the memory stick every boot. Placed AFTER the
      // allocation above, which is why it reports a real size.
      if (gu_profilingEnabled) {
          FILE *mf = fopen("mem_dbg.log", "w");
          if (mf) {
              const int32 freeMem = (int32)sceKernelTotalFreeMemSize();
              fprintf(mf, "GU_GPU_FACES        = %d\n", (int)GU_GPU_FACES);
              fprintf(mf, "free mem after init = %d bytes (%d KB)\n", (int)freeMem, (int)(freeMem / 1024));
              fprintf(mf, "gpu face verts      = %d (%d bytes)\n", (int)gu_face_vert_max,
                      (int)(gu_face_vert_max * (int)sizeof(GUFaceVertex)));
              fprintf(mf, "framebuffer         = %d bytes at %p\n", (int)PRESENT_ALLOC_BYTES, (void *)screen_pixels);
              fprintf(mf, "sizeof(GUQueueEntry)= %d, queue total = %d bytes\n", (int)sizeof(GUQueueEntry),
                      (int)(sizeof(GUQueueEntry) * GU_DRAW_QUEUE_MAX));
              fclose(mf);
          }
      }
  }
#endif

#if GU_VRAM_BENCH
  GU_VramWriteBench();
#endif
#if GU_XFORM_BENCH
  GU_XformBench();
#endif

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
  {
      if (gu_layerLogFrame < 3) {
          if (!gu_layerLog) gu_layerLog = fopen("layers.log", "w");
          ++gu_layerLogFrame;
          if (gu_layerLog) { fprintf(gu_layerLog, "--- frame %d scene=%d ---\n", (int)gu_layerLogFrame, (int)sceneInfo.listPos); fflush(gu_layerLog); }
      } else if (gu_layerLog) { fclose(gu_layerLog); gu_layerLog = NULL; }
  }
  // are deferred (Stage 0), so it must run BEFORE the transfer below.
  GU_FlushDrawQueue();

#if GU_GPU_TILES || GU_TILE_SELFTEST || GU_TILE_ATLAS_TEST
  // Rebuild when the scene changes -- the tileset is per-stage. Cheap
  // because it is once per scene, not per frame.
  ++gu_frameCounter;
#endif
#if GU_TILE_SELFTEST
  GU_TileQuadSelfTest();
#endif
#if GU_FB_DUMP
  {
      static bool sceneListDumped = false;
      if (!sceneListDumped) {
          sceneListDumped = true;
          FILE *sf = fopen("scenes.log", "w");
          if (sf) {
              fprintf(sf, "categoryCount=%d activeCategory=%d listPos=%d\n",
                      (int)sceneInfo.categoryCount, (int)sceneInfo.activeCategory, (int)sceneInfo.listPos);
              for (int32 c = 0; c < sceneInfo.categoryCount; ++c) {
                  SceneListInfo *cat = &sceneInfo.listCategory[c];
                  fprintf(sf, "cat %d '%s' start=%d count=%d\n",
                          (int)c, cat->name, (int)cat->sceneOffsetStart, (int)cat->sceneCount);
                  for (int32 e = 0; e < cat->sceneCount; ++e) {
                      SceneListEntry *se = &sceneInfo.listData[cat->sceneOffsetStart + e];
                      fprintf(sf, "    [%d] %s\n", (int)e, se->name);
                  }
              }
              fclose(sf);
          }
      }
  }
#endif
#if GU_FB_DUMP
  {
      static int32 fbDumpFrame = 0;
      if (++fbDumpFrame == GU_FB_DUMP_AT) {
          FILE *df2 = fopen("disp.ppm", "wb");
          if (df2) {
              const u16 *disp = (const u16 *)gu_dispFront;
              fprintf(df2, "P6\n%d %d\n255\n", (int)PSP_SCREEN_WIDTH, (int)PSP_SCREEN_HEIGHT);
              for (int32 y = 0; y < PSP_SCREEN_HEIGHT; ++y) {
                  for (int32 x = 0; x < PSP_SCREEN_WIDTH; ++x) {
                      const uint16 c = disp[y * PSP_LINE_SIZE + x];
                      unsigned char rgb[3];
                      rgb[0] = (unsigned char)(((c      ) & 0x1F) << 3);
                      rgb[1] = (unsigned char)(((c >>  5) & 0x3F) << 2);
                      rgb[2] = (unsigned char)(((c >> 11) & 0x1F) << 3);
                      fwrite(rgb, 1, 3, df2);
                  }
              }
              fclose(df2);
          }
          FILE *pf = fopen("fb.ppm", "wb");
          if (pf) {
              fprintf(pf, "P6\n%d %d\n255\n", (int)MANIA_WIDTH, (int)MANIA_HEIGHT);
              for (int32 y = 0; y < MANIA_HEIGHT; ++y) {
                  for (int32 x = 0; x < MANIA_WIDTH; ++x) {
                      const uint16 c = screen_pixels[y * screens[0].pitch + x];
                      // This port swapped the 565 channel order so red sits in
                      // the low bits, matching PSP GU_PSM_5650.
                      unsigned char rgb[3];
                      rgb[0] = (unsigned char)(((c      ) & 0x1F) << 3);
                      rgb[1] = (unsigned char)(((c >>  5) & 0x3F) << 2);
                      rgb[2] = (unsigned char)(((c >> 11) & 0x1F) << 3);
                      fwrite(rgb, 1, 3, pf);
                  }
              }
              fclose(pf);
          }
      }
  }
#endif
#if GU_TILE_ATLAS_TEST
  GU_DrawTileAtlasTest();
#endif

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

// Frame pacing. Averages hide judder: a steady 56fps and a 60fps stream with
// one long stall per second produce the same average and feel completely
// different. This counts how many vblanks each delivered frame actually took.
//
// Everything is kept in memory and written once at the end. Writing to the
// memory stick every 60 frames is itself a multi-millisecond stall, so a
// periodic report would manufacture the spikes it is meant to detect.
#define GU_PACE_SPIKES    16
#define GU_PACE_REPORT_AT 600    // reached in ~30s even at 20fps

static int32 gu_paceBucket[5];
static SceUInt64 gu_paceWorst = 0;
static SceUInt64 gu_paceLast  = 0;
static int32 gu_paceFrames    = 0;
static SceUInt64 gu_spikeUsec[GU_PACE_SPIKES];
static int32 gu_spikeFrame[GU_PACE_SPIKES];
static int32 gu_spikeCount = 0;
static bool gu_paceReported = false;

static void GU_SampleFramePacing()
{
    const SceUInt64 now = sceKernelGetSystemTimeWide();

    if (gu_paceLast) {
        const SceUInt64 dt = now - gu_paceLast;
        if (dt > gu_paceWorst)
            gu_paceWorst = dt;

        int32 v = (int32)((dt + 8333) / 16667); // nearest whole vblank
        if (v < 1)
            v = 1;
        if (v > 5)
            v = 5;
        ++gu_paceBucket[v - 1];

        if (v > 1 && gu_spikeCount < GU_PACE_SPIKES) {
            gu_spikeUsec[gu_spikeCount]  = dt;
            gu_spikeFrame[gu_spikeCount] = gu_paceFrames;
            ++gu_spikeCount;
        }
    }

    gu_paceLast = now;
    ++gu_paceFrames;

    if (gu_paceFrames >= GU_PACE_REPORT_AT && !gu_paceReported) {
        gu_paceReported = true;
        FILE *pf = fopen("pacing.log", "w");
        if (pf) {
            fprintf(pf, "frames sampled : %d\n", (int)gu_paceFrames);
            fprintf(pf, "1 vblank  (60fps) : %d\n", (int)gu_paceBucket[0]);
            fprintf(pf, "2 vblanks (30fps) : %d\n", (int)gu_paceBucket[1]);
            fprintf(pf, "3 vblanks         : %d\n", (int)gu_paceBucket[2]);
            fprintf(pf, "4 vblanks         : %d\n", (int)gu_paceBucket[3]);
            fprintf(pf, "5+ vblanks        : %d\n", (int)gu_paceBucket[4]);
            fprintf(pf, "worst frame       : %.2f ms\n\n", (double)gu_paceWorst / 1000.0);
            fprintf(pf, "first %d long frames (frame : ms):\n", (int)gu_spikeCount);
            for (int32 i = 0; i < gu_spikeCount; ++i)
                fprintf(pf, "  %6d : %7.2f\n", (int)gu_spikeFrame[i], (double)gu_spikeUsec[i] / 1000.0);
            fclose(pf);
        }
    }
}

static void GU_UpdateFPSCounter()
{
    static int32 frameCount = 0;
    static SceUInt64 lastTick = 0;

    GU_SampleFramePacing();

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
        FILE *f           = fopen(GU_FPS_LOG, "w");
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
#if GU_GPU_FACES
                fprintf(h, "     gpu faces: %6.1f tri  %4.1f batches  dma %5.2f  ge %5.2f  (per frame)\n",
                        (double)gu_faceTriCount / frameCount, (double)gu_faceBatchCount / frameCount,
                        (double)gu_faceDmaUsec / 1000.0 / frameCount, (double)gu_faceGeUsec / 1000.0 / frameCount);
                gu_faceDmaUsec = gu_faceGeUsec = 0;
                gu_faceBatchCount = gu_faceTriCount = 0;
                fprintf(h, "     layers: hscroll %5.2f  vscroll %5.2f  rotozoom %5.2f  basic %5.2f  |  bands/layer %5.1f\n",
                        (double)gu_layerTypeUsec[0] / 1000.0 / frameCount, (double)gu_layerTypeUsec[1] / 1000.0 / frameCount,
                        (double)gu_layerTypeUsec[2] / 1000.0 / frameCount, (double)gu_layerTypeUsec[3] / 1000.0 / frameCount,
                        gu_layerBandSamples ? (double)gu_layerBands / gu_layerBandSamples : 0.0);
                gu_layerTypeUsec[0] = gu_layerTypeUsec[1] = gu_layerTypeUsec[2] = gu_layerTypeUsec[3] = 0;
                gu_layerBands = gu_layerBandSamples = 0;
#if GU_GPU_TILES
                fprintf(h, "     gpu tiles: %6.1f quads %4.1f batches  dma %5.2f  ge %5.2f  (per frame)\n",
                        (double)gu_tileQuadCount / frameCount, (double)gu_tileBatchCount / frameCount,
                        (double)gu_tileDmaUsec / 1000.0 / frameCount, (double)gu_tileGeUsec / 1000.0 / frameCount);
                fprintf(h, "     tile declines: type %d atlas %d verts %d stale %d size %d clip %d xspan %d pal %d room %d\n",
                        (int)gu_tile_decline[0], (int)gu_tile_decline[1], (int)gu_tile_decline[2],
                        (int)gu_tile_decline[3], (int)gu_tile_decline[4], (int)gu_tile_decline[5],
                        (int)gu_tile_decline[6], (int)gu_tile_decline[7], (int)gu_tile_decline[8]);
                for (int32 di = 0; di < 9; ++di) gu_tile_decline[di] = 0;
                gu_tileDmaUsec = gu_tileGeUsec = 0;
                gu_tileBatchCount = gu_tileQuadCount = 0;
#endif
#endif
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
                    {
                        extern SceUInt64 gu_dlSortUsec, gu_dlDrawUsec, gu_dlLayerUsec;
                        extern int32 gu_dlEntityPeak, gu_dlDrawCalls;
                        fprintf(h, "     face rejects/frame: range %.1f  bufferFull %.1f  inkOrVerts %.1f  (vert buffer %d)\n",
                                (double)gu_faceRejectRange / frameCount, (double)gu_faceRejectFull / frameCount,
                                (double)gu_faceRejectInk / frameCount, (int)gu_face_vert_max);
                        gu_faceRejectRange = gu_faceRejectFull = gu_faceRejectInk = 0;
                        fprintf(h, "     drawlist: sort %6.2f  entityDraw %6.2f  layers %6.2f  | peak list %d, draws/frame %.1f\n",
                                (double)gu_dlSortUsec / 1000.0 / frameCount, (double)gu_dlDrawUsec / 1000.0 / frameCount,
                                (double)gu_dlLayerUsec / 1000.0 / frameCount, (int)gu_dlEntityPeak,
                                (double)gu_dlDrawCalls / frameCount);
                        gu_dlSortUsec = gu_dlDrawUsec = gu_dlLayerUsec = 0;
                        gu_dlEntityPeak = 0;
                        gu_dlDrawCalls = 0;
                    }
                    {
                        extern SceUInt64 gu_s3dModeUsec[3];
                        extern int32 gu_s3dModeCalls[3], gu_s3dModeFaces[3];
                        static const char *modeName[3] = { "normal    ", "no shading", "no sort   " };
                        for (int32 m = 0; m < 3; ++m)
                            fprintf(h, "     s3d %s: %8.2f us/1000 faces  (%d calls, %d faces)\n", modeName[m],
                                    gu_s3dModeFaces[m] ? (double)gu_s3dModeUsec[m] * 1000.0 / gu_s3dModeFaces[m] : 0.0,
                                    (int)gu_s3dModeCalls[m], (int)gu_s3dModeFaces[m]);
                        for (int32 m = 0; m < 3; ++m) {
                            gu_s3dModeUsec[m] = 0;
                            gu_s3dModeCalls[m] = 0;
                            gu_s3dModeFaces[m] = 0;
                        }
                    }
                    {
                        extern int32 gu_s3dVertsXf, gu_s3dFacesIn, gu_s3dFacesNear;
                        fprintf(h, "     geometry/frame: verts transformed %.0f  faces %.0f  of which dropped near-plane %.0f\n",
                                (double)gu_s3dVertsXf / frameCount, (double)gu_s3dFacesIn / frameCount,
                                (double)gu_s3dFacesNear / frameCount);
                        gu_s3dVertsXf = gu_s3dFacesIn = gu_s3dFacesNear = 0;
                    }
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

    // The present enqueued last frame was waited on in CopyFrameBuffer before
    // screen_texture was overwritten, so by now it is complete and safe to show.
    if (gu_dispPending) {
        const int setRc = sceDisplaySetFrameBuf((void *)gu_dispBack, PSP_LINE_SIZE, PSP_DISPLAY_PIXEL_FORMAT_565,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
        {
            static int32 logged = 0;
            if (logged < 4) {
                ++logged;
                FILE *dl = fopen("disp_dbg.log", logged == 1 ? "w" : "a");
                if (dl) {
                    if (logged == 1)
                        fprintf(dl, "front=%p back=%p screen_texture=%p screen_pixels=%p atlas=%p arena=%p arenaSize=%u\n",
                                (void *)gu_dispFront, (void *)gu_dispBack, (void *)screen_texture,
                                (void *)screen_pixels, (void *)gu_tile_atlas, (void *)gu_tex_arena,
                                (unsigned)gu_tex_arena_size);
                    fprintf(dl, "swap %d: setFrameBuf(%p) rc=%d (0 = ok)\n", (int)logged, (void *)gu_dispBack, setRc);
                    fclose(dl);
                }
            }
        }
        u16 *swap    = gu_dispFront;
        gu_dispFront = gu_dispBack;
        gu_dispBack  = swap;
    }
    gu_dispPending = true;

    {
        const SceUInt64 waitStart = sceKernelGetSystemTimeWide();
        sceDisplayWaitVblankStart();
        gu_vblankThisFrame = sceKernelGetSystemTimeWide() - waitStart;
        gu_vblankUsecAccum += gu_vblankThisFrame;
    }

    GE_CMD(FBP, ((u32)gu_dispBack & 0x00FFFFFF));
    GE_CMD(FBW, (((u32)gu_dispBack & 0xFF000000) >> 8) | PSP_LINE_SIZE);
    GE_CMD(TPSM, 0);  // GU_PSM_5650 -- see note above; do not remove
    GE_CMD(TMODE, 0); // no mipmaps, not swizzled
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
