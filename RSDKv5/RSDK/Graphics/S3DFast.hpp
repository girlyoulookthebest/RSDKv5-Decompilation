// Scene3D fast path for the Special Stage's shaded models.
//
// Included by Scene3D.cpp above AddModelToScene; it uses S3D_FaceIsCulled, the
// gu_s3d* counters and the ModelFlags enum defined before that point.
//
// The stock path transforms, projects and shades a vertex once per index, and
// the Special Stage's models share each vertex between ~2.4 indices. Here an
// eligible model's unique vertices are transformed once into an arena and its
// index list is kept as a "run". Draw3DScene then projects and shades each
// unique vertex once into a 12-byte record that is also a GE vertex, and emits
// the faces as indices into those records.
//
// Eligible: drawMode S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN, models flagged
// USENORMALS|USECOLOURS with a normal matrix, 3- or 4-vertex faces. A scene
// that also holds stock-path models, or turns out to use another draw mode, is
// expanded back into its vertex array (S3D_MaterializeLazy) and drawn the
// stock way.
//
// Not bit-exact with the stock path: the VFPU rounds differently (well under a
// pixel), and depth is interpolated per vertex rather than flat per face.

#if RETRO_RENDERDEVICE_GU

#define S3D_FAST      1
#define S3D_FAST_MODE S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN

#define S3D_LAZY_RUN_MAX 192

extern "C" {
// Pools for the indexed batches (GURenderDevice.cpp).
void *GU_RecReserve(int32 count);
uint16 *GU_IdxReserve(int32 want, int32 *granted);
void GU_IdxRelease(int32 unused);
void GU_QueueIdxBatch(const void *verts, int32 vertCount, const uint16 *indices, int32 indexCount);
extern int32 gu_faceBatchAvailable; // GE face batching compiled in and usable
extern int32 gu_faceDepthShiftExp;  // = GU_DEPTH_SHIFT
extern int32 gu_faceDepthFarExp;    // = GU_DEPTH_FAR
}

// A projected, shaded vertex. The layout is GUFaceVertex's (colour, then 16-bit
// x, y, z), so the GE reads these records directly.
//
// may_alias matters: when the record pool is full, pass 1 writes these in place
// over the Scene3DVertex array it is reading (record u overlaps vertex u for
// u < 2), and without it the compiler may reorder a store before the load it
// overlaps.
struct __attribute__((may_alias)) S3DDrawVertex {
    uint32 color; // 0xAABBGGRR, alpha carries the flags below
    int16 x, y;   // projected position in whole pixels
    int16 z;      // clamped 16-bit depth
    int16 pad;
};
static_assert(sizeof(S3DDrawVertex) == 12, "must match GUFaceVertex");

// Flags in the colour's alpha byte, which the face loop loads anyway. A vertex
// that reaches the GE always has alpha 0xFF, and the CPU fallback's colour
// swap drops the alpha byte.
#define S3D_A_NEAR 0x00000000u // vertex failed the z < 0x100 test
#define S3D_A_FAR  0xFE000000u // vertex is outside the GE's 2D range
#define S3D_A_OK   0xFF000000u

struct S3DLazyRun {
    uint16 *indices;
    int32 indexCount;
    int32 uniqBase;  // first slot in s3dUniq
    int32 uniqCount;
    int32 vertBase;  // slot in scn->vertices this run would have expanded into
    int32 faceVertCount;
};

static S3DLazyRun s3dLazyRuns[S3D_LAZY_RUN_MAX];
static int32 s3dLazyRunCount = 0;
static int32 s3dLazyScene    = -1; // scene currently holding the arena, -1 none
static int32 s3dLazyMixed    = 0;  // this scene also has stock-expanded models

// Per scene, per Prepare cycle: the stock path has written vertices into it.
static uint8 s3dStockWrote[SCENE3D_COUNT];

// Transformed unique vertices, projected and shaded in place at draw time.
static Scene3DVertex *s3dUniq = NULL;
static int32 s3dUniqCap       = 0;
static int32 s3dUniqUsed      = 0;
static int32 s3dLazyInitDone  = 0;

// Per-model bound on the unique vertex range (max index + 1), cached and
// re-derived if the model is reloaded.
static uint16 *s3dBoundIdx[MODEL_COUNT];
static int32 s3dBoundVal[MODEL_COUNT];

// Counters, reported per profile window.
static int32 s3dLazyRunsDone = 0, s3dLazyUniqDone = 0, s3dLazyIdxDone = 0;
static int32 s3dLazyBusy = 0, s3dLazyNoFit = 0, s3dLazyNoRun = 0;
static int32 s3dLazyMaterialized = 0, s3dLazyMixedFrames = 0;
static int32 s3dLazyFallbackFaces = 0;
static int32 s3dIdxReserves = 0, s3dIdxFaces = 0;

// Draw time split into pass 1 (project and shade) and pass 2 (faces).
static SceUInt64 s3dProjUsec = 0;
static SceUInt64 s3dFaceUsec = 0;
static int32 s3dProjVerts    = 0;

static bool32 S3D_LazyInit()
{
    if (s3dLazyInitDone)
        return s3dUniq != NULL;

    s3dLazyInitDone = 1;

    // Heap, and progressively smaller: a static array this size once broke
    // the framebuffer allocation on hardware. The fast path stays off if
    // nothing fits.
    static const int32 tryCap[] = { SCENE3D_VERT_COUNT, 3072, 2048, 1024 };
    for (int32 i = 0; i < (int32)(sizeof(tryCap) / sizeof(tryCap[0])); ++i) {
        s3dUniq = (Scene3DVertex *)malloc(sizeof(Scene3DVertex) * tryCap[i]);
        if (s3dUniq) {
            s3dUniqCap = tryCap[i];
            break;
        }
    }
    return s3dUniq != NULL;
}

void RSDK::S3D_LazyReset(uint16 sceneID)
{
    if (sceneID < SCENE3D_COUNT)
        s3dStockWrote[sceneID] = 0;

    // Preparing the owning scene releases the arena; preparing another scene
    // must not, since its runs are still waiting to be drawn.
    if (s3dLazyScene == (int32)sceneID) {
        s3dLazyScene    = -1;
        s3dLazyRunCount = 0;
        s3dUniqUsed     = 0;
        s3dLazyMixed    = 0;
    }
}

static int32 S3D_ModelUniqBound(uint16 id, Model *mdl)
{
    if (s3dBoundIdx[id] == mdl->indices && s3dBoundVal[id] != 0)
        return s3dBoundVal[id];

    int32 mx = -1;
    for (int32 i = 0; i < mdl->indexCount; ++i) {
        const int32 v = mdl->indices[i];
        if (v > mx)
            mx = v;
    }

    int32 bound = mx + 1;
    if (bound <= 0 || bound > mdl->vertCount)
        bound = -1; // index past the vertex array: leave this model alone

    s3dBoundIdx[id] = mdl->indices;
    s3dBoundVal[id] = bound;
    return bound;
}

// Called when a model is NOT taken by the fast path. A scene that ends up with
// both kinds of content is drawn by the stock path.
static void S3D_LazyPass(Scene3D *scn, int32 sceneIndex, int32 indCnt)
{
    // The stock path drops a model that does not fit, writing nothing.
    if (scn->vertLimit - scn->vertexCount < indCnt)
        return;

    s3dStockWrote[sceneIndex] = 1;
    if (s3dLazyScene == sceneIndex)
        s3dLazyMixed = 1;
}

// Returns the unique vertex count, or -1 to stay on the stock path. The arena
// is only claimed once every check has passed.
static int32 S3D_LazyAdmit(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matNormals, Model *mdl, Scene3D *scn)
{
    if (drawMode != S3D_FAST_MODE || !matNormals || !mdl->colors)
        return -1;
    if (mdl->flags != (MODEL_USENORMALS | MODEL_USECOLOURS))
        return -1;

    const int32 fvc = mdl->faceVertCount;
    if ((fvc != 3 && fvc != 4) || mdl->indexCount <= 0 || (mdl->indexCount % fvc) != 0)
        return -1;
    if (!S3D_LazyInit())
        return -1;

    // Already has stock content this cycle: it would only be materialized.
    if (s3dStockWrote[sceneIndex])
        return -1;

    const bool32 owner = s3dLazyScene == (int32)sceneIndex;
    if (!owner && s3dLazyScene >= 0) {
        ++s3dLazyBusy; // another scene holds the arena
        return -1;
    }
    if (owner && s3dLazyMixed)
        return -1;

    // Out of vertex slots: let the stock path drop the model, and log it, as before.
    if (scn->vertLimit - scn->vertexCount < mdl->indexCount)
        return -1;

    const int32 runBase  = owner ? s3dLazyRunCount : 0;
    const int32 uniqBase = owner ? s3dUniqUsed : 0;

    if (runBase >= S3D_LAZY_RUN_MAX) {
        ++s3dLazyNoRun;
        return -1;
    }

    const int32 uniqCount = S3D_ModelUniqBound(modelFrames, mdl);
    if (uniqCount <= 0)
        return -1;
    if (uniqBase + uniqCount > s3dUniqCap) {
        ++s3dLazyNoFit;
        return -1;
    }

    if (!owner) {
        s3dLazyScene    = sceneIndex;
        s3dLazyRunCount = 0;
        s3dUniqUsed     = 0;
        s3dLazyMixed    = 0;
    }

    return uniqCount;
}

// Books the run and advances the scene exactly as the stock path would have.
static Scene3DVertex *S3D_LazyBook(Scene3D *scn, Model *mdl, int32 uniqCount)
{
    const int32 indCnt = mdl->indexCount;
    const int32 fvc    = mdl->faceVertCount;
    const int32 faces  = indCnt / fvc;

    S3DLazyRun *run    = &s3dLazyRuns[s3dLazyRunCount++];
    run->indices       = mdl->indices;
    run->indexCount    = indCnt;
    run->uniqBase      = s3dUniqUsed;
    run->uniqCount     = uniqCount;
    run->vertBase      = scn->vertexCount;
    run->faceVertCount = fvc;

    // Still filled: the stock draw path reads it if the scene is materialized.
    memset(&scn->faceVertCounts[scn->faceCount], (uint8)fvc, (size_t)faces);

    scn->vertexCount += indCnt;
    scn->faceCount += faces; // scn->drawMode is set by the caller

    gu_s3dVertsXf += uniqCount;
    s3dUniqUsed += uniqCount;
    ++s3dLazyRunsDone;
    s3dLazyUniqDone += uniqCount;
    s3dLazyIdxDone += indCnt;

    return &s3dUniq[run->uniqBase];
}

// --- VFPU transform ------------------------------------------------------
//
// One vhtfm4 does the 3x4 transform and one vdot the shaded normal's y. A
// ModelVertex is read as two quads ([x y z nx] and [z nx ny nz]) and x, y, z,
// ny are written as one quad, which is the front of a Scene3DVertex.
//
// vhtfm4 takes the matrix rows from the column registers, so row r is loaded
// into C1r0. The fourth row is the normal matrix's y row as [0 mn10 mn11 mn12],
// which dotted with [z nx ny nz] gives ny.
//
// The integer path floors each product before summing; this sums in float and
// floors once, a difference of a few 16.16 units. S3D_VFPU 0 is the integer
// path, for reference.
#ifndef S3D_VFPU
#define S3D_VFPU 1
#endif

#if S3D_VFPU
struct S3DVfpuMat {
    float row[4][4]; // world rows 0-2 with the translation in column 3; row 3 = normal y row
    float lerp;      // animator timer / 256 for mesh frames
    float pad[3];
} __attribute__((aligned(16)));

static inline void S3D_VfpuLoadMatrix(S3DVfpuMat *m, const Matrix *matWorld, const Matrix *matNormals, int32 interpolate)
{
    const float k = 1.0f / 256.0f;
    for (int32 r = 0; r < 3; ++r) {
        m->row[r][0] = (float)matWorld->values[r][0] * k;
        m->row[r][1] = (float)matWorld->values[r][1] * k;
        m->row[r][2] = (float)matWorld->values[r][2] * k;
        m->row[r][3] = (float)matWorld->values[r][3];
    }
    m->row[3][0] = 0.0f;
    m->row[3][1] = (float)matNormals->values[1][0] * k;
    m->row[3][2] = (float)matNormals->values[1][1] * k;
    m->row[3][3] = (float)matNormals->values[1][2] * k;
    m->lerp      = (float)interpolate * k;
    __asm__ volatile(
        "lv.q C100, 0(%0)\n\t"
        "lv.q C110, 16(%0)\n\t"
        "lv.q C120, 32(%0)\n\t"
        "lv.q C130, 48(%0)\n\t"
        "lv.s S230, 64(%0)\n\t"
        :
        : "r"(m)
        : "memory");
}

// One vertex: src is a ModelVertex, dst the front 16 bytes of a Scene3DVertex.
#define S3D_VFPU_XFORM(src, dst)                                                                                                       \
    __asm__ volatile("ulv.q C010, 0(%0)\n\t"        /* x y z nx    */                                                                \
                     "ulv.q C020, 8(%0)\n\t"        /* z nx ny nz  */                                                                \
                     "vi2f.q C010, C010, 0\n\t"                                                                                        \
                     "vi2f.q C020, C020, 0\n\t"                                                                                        \
                     "vhtfm4.q C000, M100, C010\n\t" /* rows . [x y z 1]                      */                                      \
                     "vdot.q S003, C130, C020\n\t"   /* ny = mn10 nx + mn11 ny + mn12 nz      */                                      \
                     "vf2id.q C000, C000, 0\n\t"     /* floor, as >> 8 does                   */                                      \
                     "usv.q C000, 0(%1)\n\t"                                                                                           \
                     :                                                                                                                 \
                     : "r"(src), "r"(dst)                                                                                              \
                     : "memory")

// Mesh frame: the vertex is fv + lerp * (nv - fv) on all six components first.
#define S3D_VFPU_XFORM_LERP(fv, nv, dst)                                                                                               \
    __asm__ volatile("ulv.q C010, 0(%0)\n\t"                                                                                           \
                     "ulv.q C020, 8(%0)\n\t"                                                                                           \
                     "ulv.q C200, 0(%1)\n\t"                                                                                           \
                     "ulv.q C210, 8(%1)\n\t"                                                                                           \
                     "vi2f.q C010, C010, 0\n\t"                                                                                        \
                     "vi2f.q C020, C020, 0\n\t"                                                                                        \
                     "vi2f.q C200, C200, 0\n\t"                                                                                        \
                     "vi2f.q C210, C210, 0\n\t"                                                                                        \
                     "vsub.q C200, C200, C010\n\t"                                                                                     \
                     "vsub.q C210, C210, C020\n\t"                                                                                     \
                     "vscl.q C200, C200, S230\n\t"                                                                                     \
                     "vscl.q C210, C210, S230\n\t"                                                                                     \
                     "vadd.q C010, C010, C200\n\t"                                                                                     \
                     "vadd.q C020, C020, C210\n\t"                                                                                     \
                     "vhtfm4.q C000, M100, C010\n\t"                                                                                   \
                     "vdot.q S003, C130, C020\n\t"                                                                                     \
                     "vf2id.q C000, C000, 0\n\t"                                                                                       \
                     "usv.q C000, 0(%2)\n\t"                                                                                           \
                     :                                                                                                                 \
                     : "r"(fv), "r"(nv), "r"(dst)                                                                                      \
                     : "memory")

static int32 s3dVfpuVerts = 0; // vertices transformed on the VFPU (profile window)
#endif

static bool32 S3D_TryAddModelLazy(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, color color)
{
    if (!matWorld || modelFrames >= MODEL_COUNT || sceneIndex >= SCENE3D_COUNT)
        return false;

    Model *mdl   = &modelList[modelFrames];
    Scene3D *scn = &scene3DList[sceneIndex];

    const int32 uniqCount = S3D_LazyAdmit(modelFrames, sceneIndex, drawMode, matNormals, mdl, scn);
    if (uniqCount < 0) {
        S3D_LazyPass(scn, sceneIndex, mdl->indexCount);
        return false;
    }

    const int32 mw00 = matWorld->values[0][0], mw01 = matWorld->values[0][1], mw02 = matWorld->values[0][2], mw03 = matWorld->values[0][3];
    const int32 mw10 = matWorld->values[1][0], mw11 = matWorld->values[1][1], mw12 = matWorld->values[1][2], mw13 = matWorld->values[1][3];
    const int32 mw20 = matWorld->values[2][0], mw21 = matWorld->values[2][1], mw22 = matWorld->values[2][2], mw23 = matWorld->values[2][3];
    const int32 mn10 = matNormals->values[1][0], mn11 = matNormals->values[1][1], mn12 = matNormals->values[1][2];

    Scene3DVertex *dst = S3D_LazyBook(scn, mdl, uniqCount);
    scn->drawMode      = drawMode;

    // The integer version is the MODEL_USENORMALS|MODEL_USECOLOURS branch of
    // AddModelToScene, term for term, reading the vertices in order.
    const ModelVertex *src = mdl->vertices;
    const Color *col       = mdl->colors;
#if S3D_VFPU
    {
        S3DVfpuMat vm;
        S3D_VfpuLoadMatrix(&vm, matWorld, matNormals, 0);
        for (int32 u = 0; u < uniqCount; ++u) {
            S3D_VFPU_XFORM(&src[u], &dst[u]);
            dst[u].color = col[u].color;
        }
        s3dVfpuVerts += uniqCount;
        (void)mw00; (void)mw01; (void)mw02; (void)mw03; (void)mw10; (void)mw11; (void)mw12; (void)mw13;
        (void)mw20; (void)mw21; (void)mw22; (void)mw23; (void)mn10; (void)mn11; (void)mn12;
    }
#else
    for (int32 u = 0; u < uniqCount; ++u) {
        const int32 x = src[u].x, y = src[u].y, z = src[u].z;

        dst[u].x = mw03 + (mw02 * z >> 8) + (y * mw01 >> 8) + (mw00 * x >> 8);
        dst[u].y = mw13 + (mw12 * z >> 8) + (y * mw11 >> 8) + (mw10 * x >> 8);
        dst[u].z = mw23 + (x * mw20 >> 8) + (y * mw21 >> 8) + (mw22 * z >> 8);

        dst[u].ny = (mn10 * src[u].nx >> 8) + (src[u].ny * mn11 >> 8) + (mn12 * src[u].nz >> 8);

        dst[u].color = col[u].color;
    }
#endif

    (void)color; // this branch paints from the model's own colours
    return true;
}

static bool32 S3D_TryAddMeshFrameLazy(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld,
                                      Matrix *matNormals, color color)
{
    if (!matWorld || !animator || modelFrames >= MODEL_COUNT || sceneIndex >= SCENE3D_COUNT)
        return false;

    Model *mdl   = &modelList[modelFrames];
    Scene3D *scn = &scene3DList[sceneIndex];

    const int32 uniqCount = S3D_LazyAdmit(modelFrames, sceneIndex, drawMode, matNormals, mdl, scn);
    if (uniqCount < 0) {
        S3D_LazyPass(scn, sceneIndex, mdl->indexCount);
        return false;
    }

    const int32 mw00 = matWorld->values[0][0], mw01 = matWorld->values[0][1], mw02 = matWorld->values[0][2], mw03 = matWorld->values[0][3];
    const int32 mw10 = matWorld->values[1][0], mw11 = matWorld->values[1][1], mw12 = matWorld->values[1][2], mw13 = matWorld->values[1][3];
    const int32 mw20 = matWorld->values[2][0], mw21 = matWorld->values[2][1], mw22 = matWorld->values[2][2], mw23 = matWorld->values[2][3];
    const int32 mn10 = matNormals->values[1][0], mn11 = matNormals->values[1][1], mn12 = matNormals->values[1][2];

    int32 nextFrame = animator->frameID + 1;
    if (nextFrame >= animator->frameCount)
        nextFrame = animator->loopIndex;

    const int32 frameOffset     = animator->frameID * mdl->vertCount;
    const int32 nextFrameOffset = nextFrame * mdl->vertCount;
    const int32 interpolate     = animator->timer;

    Scene3DVertex *dst = S3D_LazyBook(scn, mdl, uniqCount);
    scn->drawMode      = drawMode;

    // As above, from AddMeshFrameToScene, interpolation included.
    const ModelVertex *fv = &mdl->vertices[frameOffset];
    const ModelVertex *nv = &mdl->vertices[nextFrameOffset];
    const Color *col      = mdl->colors;
#if S3D_VFPU
    {
        S3DVfpuMat vm;
        S3D_VfpuLoadMatrix(&vm, matWorld, matNormals, interpolate);
        for (int32 u = 0; u < uniqCount; ++u) {
            S3D_VFPU_XFORM_LERP(&fv[u], &nv[u], &dst[u]);
            dst[u].color = col[u].color;
        }
        s3dVfpuVerts += uniqCount;
        (void)mw00; (void)mw01; (void)mw02; (void)mw03; (void)mw10; (void)mw11; (void)mw12; (void)mw13;
        (void)mw20; (void)mw21; (void)mw22; (void)mw23; (void)mn10; (void)mn11; (void)mn12;
    }
#else
    for (int32 u = 0; u < uniqCount; ++u) {
        const int32 x  = fv[u].x + ((interpolate * (nv[u].x - fv[u].x)) >> 8);
        const int32 y  = fv[u].y + ((interpolate * (nv[u].y - fv[u].y)) >> 8);
        const int32 z  = fv[u].z + ((interpolate * (nv[u].z - fv[u].z)) >> 8);
        const int32 nx = fv[u].nx + ((interpolate * (nv[u].nx - fv[u].nx)) >> 8);
        const int32 ny = fv[u].ny + ((interpolate * (nv[u].ny - fv[u].ny)) >> 8);
        const int32 nz = fv[u].nz + ((interpolate * (nv[u].nz - fv[u].nz)) >> 8);

        dst[u].x = mw03 + (mw02 * z >> 8) + (y * mw01 >> 8) + (mw00 * x >> 8);
        dst[u].y = mw13 + (mw12 * z >> 8) + (y * mw11 >> 8) + (mw10 * x >> 8);
        dst[u].z = mw23 + (x * mw20 >> 8) + (y * mw21 >> 8) + (mw22 * z >> 8);

        dst[u].ny = (mn10 * nx >> 8) + (ny * mn11 >> 8) + (mn12 * nz >> 8);

        dst[u].color = col[u].color;
    }
#endif

    (void)color;
    return true;
}

// Expands every run back into the scene's vertex array, in the layout the stock
// draw path expects.
static void S3D_MaterializeLazy(Scene3D *scn)
{
    for (int32 r = 0; r < s3dLazyRunCount; ++r) {
        const S3DLazyRun *run   = &s3dLazyRuns[r];
        const Scene3DVertex *uv = &s3dUniq[run->uniqBase];
        Scene3DVertex *dst      = &scn->vertices[run->vertBase];
        const uint16 *idx       = run->indices;

        for (int32 i = 0; i < run->indexCount; ++i) dst[i] = uv[idx[i]];
    }

    ++s3dLazyMaterialized;
    s3dLazyRunCount = 0;
    s3dLazyScene    = -1;
    s3dUniqUsed     = 0;
    s3dLazyMixed    = 0;
}

static bool32 S3D_TryDrawLazy(uint16 sceneID, Scene3D *scn, Entity *entity)
{
    if (s3dLazyScene != (int32)sceneID || s3dLazyRunCount == 0)
        return false;

    if (scn->drawMode != S3D_FAST_MODE || s3dLazyMixed || GU_S3D_KEEP_SORT) {
        if (s3dLazyMixed)
            ++s3dLazyMixedFrames;
        S3D_MaterializeLazy(scn);
        return false;
    }

    const int32 projX   = scn->projectionX, projY = scn->projectionY;
    const int32 diffX   = scn->diffuseX, diffY = scn->diffuseY, diffZ = scn->diffuseZ;
    const int32 diffIX  = scn->diffuseIntensityX, diffIY = scn->diffuseIntensityY, diffIZ = scn->diffuseIntensityZ;
    const int32 specIX  = scn->specularIntensityX, specIY = scn->specularIntensityY, specIZ = scn->specularIntensityZ;
    const int32 centerX = currentScreen->center.x, centerY = currentScreen->center.y;

    const int32 alpha = entity->alpha;
    const int32 ink   = entity->inkEffect;

    // The GE batch only takes INK_NONE; with any other ink every face goes to
    // DrawBlendedFace, as on the stock path.
    const int32 fastEmit    = gu_faceBatchAvailable && ink == INK_NONE;
    const int32 depthShift  = gu_faceDepthShiftExp;
    const int32 depthFar    = gu_faceDepthFarExp;

    for (int32 r = 0; r < s3dLazyRunCount; ++r) {
        const S3DLazyRun *run = &s3dLazyRuns[r];
        Scene3DVertex *uv     = &s3dUniq[run->uniqBase];
        const int32 uc        = run->uniqCount;

        const SceUInt64 tp0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

#if S3D_VFPU
        {
            static float __attribute__((aligned(16))) s3dProjScale[4];
            s3dProjScale[0] = (float)(1 << projX);
            s3dProjScale[1] = (float)(1 << projY);
            s3dProjScale[2] = 0.0f;
            s3dProjScale[3] = 0.0f;
            __asm__ volatile("lv.q C020, 0(%0)\n\t" : : "r"(s3dProjScale) : "memory");
        }
#endif
        // --- pass 1: project and shade each unique vertex into a record -----
        //
        // Records go to a pool that lives until the flush, since the arena is
        // released by the next Prepare3DScene. If the pool is full they are
        // written in place over the arena and every face takes the CPU
        // fallback.
        S3DDrawVertex *dv    = fastEmit ? (S3DDrawVertex *)GU_RecReserve(uc) : NULL;
        const bool recInPool = dv != NULL;
        if (!dv)
            dv = (S3DDrawVertex *)uv;

        for (int32 u = 0; u < uc; ++u) {
            const int32 vertZ = uv[u].z;
            if (vertZ < 0x100) {
                dv[u].color = S3D_A_NEAR;
                continue;
            }

#if S3D_VFPU
            // x * 2^projX / z with one reciprocal, truncated toward zero as C
            // division is. Float rounding can flip the truncation when the
            // quotient is within ~1e-4 of an integer: one pixel, rarely.
            int32 pxi, pyi;
            __asm__ volatile("mtv %2, S000\n\t"
                             "mtv %3, S001\n\t"
                             "mtv %4, S002\n\t"
                             "vi2f.t C000, C000, 0\n\t"
                             "vrcp.s S010, S002\n\t"
                             "vmul.p C000, C000, C020\n\t" /* x * 2^projX, y * 2^projY */
                             "vscl.p C000, C000, S010\n\t" /* / z */
                             "vf2iz.p C000, C000, 0\n\t"
                             "mfv %0, S000\n\t"
                             "mfv %1, S001\n\t"
                             : "=r"(pxi), "=r"(pyi)
                             : "r"(uv[u].x), "r"(uv[u].y), "r"(vertZ));
            const int32 px = (centerX + pxi) << 16;
            const int32 py = (centerY - pyi) << 16;
#else
            const int32 px = (centerX << 16) + ((uv[u].x << projX) / vertZ << 16);
            const int32 py = (centerY << 16) - ((uv[u].y << projY) / vertZ << 16);
#endif

            const int32 normal    = uv[u].ny;
            const int32 normalVal = (normal >> 2) * (abs(normal) >> 2);
            const uint32 srcClr   = uv[u].color;

            int32 specular = normalVal >> 6 >> specIX;
            specular       = CLAMP(specular, 0x00, 0xFF);
            int32 cr       = specular + ((int32)((srcClr >> 16) & 0xFF) * ((normal >> 10) + diffX) >> diffIX);

            specular = normalVal >> 6 >> specIY;
            specular = CLAMP(specular, 0x00, 0xFF);
            int32 cg = specular + ((int32)((srcClr >> 8) & 0xFF) * ((normal >> 10) + diffY) >> diffIY);

            specular = normalVal >> 6 >> specIZ;
            specular = CLAMP(specular, 0x00, 0xFF);
            int32 cb = specular + ((int32)((srcClr >> 0) & 0xFF) * ((normal >> 10) + diffZ) >> diffIZ);

            cr = CLAMP(cr, 0x00, 0xFF);
            cg = CLAMP(cg, 0x00, 0xFF);
            cb = CLAMP(cb, 0x00, 0xFF);

            // The projection is a whole number of pixels, so >> 16 is lossless.
            const int32 ix = px >> 16;
            const int32 iy = py >> 16;

            dv[u].color = ((ix < -1024 || ix > 2047 || iy < -1024 || iy > 2047) ? S3D_A_FAR : S3D_A_OK)
                          | ((uint32)cb << 16) | ((uint32)cg << 8) | (uint32)cr;
            dv[u].x     = (int16)ix;
            dv[u].y     = (int16)iy;
            {
                int32 dz = vertZ >> depthShift;
                if (dz > depthFar)
                    dz = depthFar;
                dv[u].z   = (int16)dz; // vertZ >= 0x100 here, so never negative
                dv[u].pad = 0;
            }
        }

        if (gu_profilingEnabled) {
            s3dProjUsec += sceKernelGetSystemTimeWide() - tp0;
            s3dProjVerts += uc;
        }
        const SceUInt64 tf0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0;

        // --- pass 2: faces as indices into the records ----------------------
        //
        // No cull here: the GE draws nothing for a zero-area triangle, and back
        // faces were never culled. A face outside the GE's 2D range goes to
        // DrawBlendedFace, after the indices before it are queued, so it still
        // lands on top of them.
        const uint16 *idx   = run->indices;
        const int32 fvc     = run->faceVertCount;
        const int32 faceCnt = run->indexCount / fvc;
        const int32 needed  = (fvc == 3) ? 3 : 6;

        // Back-wound faces fill the block from the start and front-wound faces
        // from the end, and the two halves are queued in that order. With
        // per-vertex depth a back face ties with its front neighbour along the
        // shared edge, and the depth test gives the tie to whichever is drawn
        // later: this keeps the front face on top.
        uint16 *ib    = NULL;
        int32 ibCap   = 0;
        int32 ibUsed  = 0; // back-wound indices, from the block's start
        int32 ibFront = 0; // front-wound indices, from the block's end down to here
        int32 ibFaces = 0;
        if (fastEmit && recInPool) {
            ib      = GU_IdxReserve(faceCnt * needed, &ibCap);
            ibFront = ibCap;
            ++s3dIdxReserves;
        }

        for (int32 f = 0, i = 0; f < faceCnt; ++f, i += fvc) {
            ++gu_s3dFacesIn;

            int32 nearHit  = 0;
            int32 outRange = 0;
            for (int32 v = 0; v < fvc; ++v) {
                const uint32 c = dv[idx[i + v]].color;
                if (c < S3D_A_FAR) { // alpha 0x00
                    nearHit = 1;
                    ++gu_s3dFacesNear;
                    break;
                }
                if (c < S3D_A_OK) // alpha 0xFE
                    outRange = 1;
            }
            if (nearHit)
                continue;

            if (!outRange && ib && ibUsed + needed <= ibFront) {
                const S3DDrawVertex *q0 = &dv[idx[i]], *q1 = &dv[idx[i + 1]], *q2 = &dv[idx[i + 2]];
                int32 cross = (q1->x - q0->x) * (q2->y - q0->y) - (q1->y - q0->y) * (q2->x - q0->x);
                if (cross == 0 && fvc == 4) {
                    const S3DDrawVertex *q3 = &dv[idx[i + 3]];
                    cross = (q2->x - q0->x) * (q3->y - q0->y) - (q2->y - q0->y) * (q3->x - q0->x);
                }
                if (cross == 0)
                    continue; // zero area
                uint16 *o;
                if (cross < 0) {
                    o = &ib[ibUsed];
                    ibUsed += needed;
                }
                else {
                    ibFront -= needed;
                    o = &ib[ibFront];
                }
                o[0] = idx[i];
                o[1] = idx[i + 1];
                o[2] = idx[i + 2];
                if (fvc == 4) {
                    o[3] = idx[i];
                    o[4] = idx[i + 2];
                    o[5] = idx[i + 3];
                }
                ++ibFaces;
                ++s3dIdxFaces;
                continue;
            }

            // Fallback: queue the indices so far, then hand the face to the CPU.
            if (ib) {
                if (ibFaces) {
                    GU_QueueIdxBatch(dv, uc, ib, ibUsed);
                    GU_QueueIdxBatch(dv, uc, &ib[ibFront], ibCap - ibFront);
                }
                else
                    GU_IdxRelease(ibCap);
                ib      = NULL;
                ibCap   = 0;
                ibUsed  = 0;
                ibFront = 0;
                ibFaces = 0;
            }

            Vector2 vertPos[4];
            uint32 vertClrs[4];
            int32 depth = 0;
            for (int32 v = 0; v < fvc; ++v) {
                const S3DDrawVertex *qv = &dv[idx[i + v]];
                vertPos[v].x            = (int32)qv->x << 16;
                vertPos[v].y            = (int32)qv->y << 16;
                const uint32 c          = qv->color; // GE order
                vertClrs[v]             = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                depth += (int32)qv->z;
            }
            if (S3D_FaceIsCulled(vertPos, fvc))
                continue;
            ++s3dLazyFallbackFaces;
            gu_faceDepth = (depth / fvc) << depthShift;
            DrawBlendedFace(vertPos, vertClrs, fvc, alpha, ink);

            // DrawBlendedFace may have flushed the queue, which resets the index
            // pool: reserve again for the rest of the run.
            if (fastEmit && recInPool && f + 1 < faceCnt) {
                ib      = GU_IdxReserve((faceCnt - f - 1) * needed, &ibCap);
                ibFront = ibCap;
                ++s3dIdxReserves;
            }
        }
        if (ib) {
            if (ibFaces) {
                GU_QueueIdxBatch(dv, uc, ib, ibUsed);
                GU_QueueIdxBatch(dv, uc, &ib[ibFront], ibCap - ibFront);
            }
            else
                GU_IdxRelease(ibCap);
        }

        if (gu_profilingEnabled)
            s3dFaceUsec += sceKernelGetSystemTimeWide() - tf0;
    }

    s3dLazyRunCount = 0;
    s3dLazyScene    = -1;
    s3dUniqUsed     = 0;
    return true;
}

// Called once per frame before the draw queue is flushed. A scene that was
// prepared but never drawn must not hold the arena across frames.
extern "C" void S3D_LazyFrameTick()
{
    s3dLazyScene    = -1;
    s3dLazyRunCount = 0;
    s3dUniqUsed     = 0;
    s3dLazyMixed    = 0;
}

extern "C" void S3D_LazyReport(FILE *h, double frameCount)
{
    fprintf(h, "     s3d lazy: runs %.1f/frame  uniq %.0f  idx %.0f  (dedupe %.2fx)  fallback faces %.0f\n", s3dLazyRunsDone / frameCount,
            s3dLazyUniqDone / frameCount, s3dLazyIdxDone / frameCount,
            s3dLazyUniqDone ? (double)s3dLazyIdxDone / s3dLazyUniqDone : 0.0, s3dLazyFallbackFaces / frameCount);
    fprintf(h, "     s3d draw split: proj+shade %5.2f ms (%.0f verts, %.3f us/vert)  faceloop %5.2f ms\n",
            (double)s3dProjUsec / 1000.0 / frameCount, s3dProjVerts / frameCount,
            s3dProjVerts ? (double)s3dProjUsec / s3dProjVerts : 0.0, (double)s3dFaceUsec / 1000.0 / frameCount);
#if S3D_VFPU
    fprintf(h, "     s3d vfpu: %.0f verts/frame\n", s3dVfpuVerts / frameCount);
    s3dVfpuVerts = 0;
#endif
    fprintf(h, "     s3d idx: %.1f reserves/frame  %.0f faces/frame\n", s3dIdxReserves / frameCount, s3dIdxFaces / frameCount);
    fprintf(h, "     s3d lazy: busy %d  nofit %d  norun %d  materialized %d  mixed %d  arena %d\n", (int)s3dLazyBusy, (int)s3dLazyNoFit,
            (int)s3dLazyNoRun, (int)s3dLazyMaterialized, (int)s3dLazyMixedFrames, (int)s3dUniqCap);

    s3dLazyRunsDone = s3dLazyUniqDone = s3dLazyIdxDone = 0;
    s3dLazyBusy = s3dLazyNoFit = s3dLazyNoRun = 0;
    s3dLazyMaterialized = s3dLazyMixedFrames = s3dLazyFallbackFaces = 0;
    s3dIdxReserves = s3dIdxFaces = 0;
    s3dProjUsec = s3dFaceUsec = 0;
    s3dProjVerts = 0;
}

#else // !RETRO_RENDERDEVICE_GU

#define S3D_FAST 0
void RSDK::S3D_LazyReset(uint16 sceneID) { (void)sceneID; }

#endif
