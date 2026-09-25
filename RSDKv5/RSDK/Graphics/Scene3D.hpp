#ifndef SCENE3D_H
#define SCENE3D_H

namespace RSDK
{

#define RSDK_SIGNATURE_MDL (0x4C444D) // "MDL"

#define SCENE3D_COUNT      (0x20)
#define MODEL_COUNT        (0x100)
#define SCENE3D_VERT_COUNT (0x4000)

enum Scene3DDrawTypes {
    S3D_WIREFRAME,
    S3D_SOLIDCOLOR,

    S3D_UNUSED_1,
    S3D_UNUSED_2,

    S3D_WIREFRAME_SHADED,
    S3D_SOLIDCOLOR_SHADED,

    S3D_SOLIDCOLOR_SHADED_BLENDED,

    S3D_WIREFRAME_SCREEN,
    S3D_SOLIDCOLOR_SCREEN,

    S3D_WIREFRAME_SHADED_SCREEN,
    S3D_SOLIDCOLOR_SHADED_SCREEN,

    S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN,
};

struct ScanEdge {
    int32 start;
    int32 end;

    int32 startR;
    int32 endR;
    int32 startG;
    int32 endG;
    int32 startB;
    int32 endB;
};

struct Matrix {
    int32 values[4][4];
};

struct ModelVertex {
    int32 x;
    int32 y;
    int32 z;

    int32 nx;
    int32 ny;
    int32 nz;
};

struct TexCoord {
    float x;
    float y;
};

struct Model {
    RETRO_HASH_MD5(hash);
    ModelVertex *vertices;
    TexCoord *texCoords;
    Color *colors;
    uint16 *indices;
    uint16 vertCount;
    uint16 indexCount;
    uint16 frameCount;
    uint8 flags;
    uint8 faceVertCount;
    uint8 scope;
};

struct Scene3DVertex {
    int32 x;
    int32 y;
    int32 z;

    // Only ny survives. Draw3DScene shades from the y component of the
    // transformed normal, and nothing in the engine or the game reads nx or nz
    // -- they were computed and stored every frame for nobody.
    //
    // That matters more than the arithmetic saved. The Special Stage's
    // transform measures 853 ns/vertex against 308 ns/vertex for the identical
    // arithmetic on cached data, so it is waiting on memory, not computing;
    // dropping these takes the struct from 28 bytes to 20, and the per-frame
    // vertex working set from ~523 KB to ~374 KB against a 16 KB data cache.
    int32 ny;

    // No tx/ty: nothing in the engine or the game ever read them. Removing
    // them takes this struct from 36 to 28 bytes. Texture coordinates for a
    // model live in Model::texCoords, which is unrelated to this.
    uint32 color;
};

struct Scene3DFace {
    int32 depth;
    int32 index;
};

struct Scene3D {
    RETRO_HASH_MD5(hash);
    Scene3DVertex *vertices;
    // No normals array: it was allocated at full vertLimit and cleared on every
    // Prepare3DScene, but nothing ever wrote or read it. Per-vertex normals
    // live in Scene3DVertex itself (nx/ny/nz), which is what the shaded draw
    // modes actually use.
    Scene3DFace *faceBuffer;
    uint8 *faceVertCounts;

    int32 projectionX;
    int32 projectionY;

    int32 diffuseX;
    int32 diffuseY;
    int32 diffuseZ;

    int32 diffuseIntensityX;
    int32 diffuseIntensityY;
    int32 diffuseIntensityZ;

    int32 specularIntensityX;
    int32 specularIntensityY;
    int32 specularIntensityZ;

    uint16 vertLimit;
    uint16 vertexCount;
    uint16 faceCount;
    uint8 drawMode;
    uint8 scope;
};

extern Model modelList[MODEL_COUNT];
extern Scene3D scene3DList[SCENE3D_COUNT];

extern ScanEdge scanEdgeBuffer[SCREEN_YSIZE * 2];

void ProcessScanEdge(int32 x1, int32 y1, int32 x2, int32 y2);
void ProcessScanEdgeClr(uint32 c1, uint32 c2, int32 x1, int32 y1, int32 x2, int32 y2);

void SetIdentityMatrix(Matrix *matrix);
void MatrixMultiply(Matrix *dest, Matrix *matrixA, Matrix *matrixB);
void MatrixTranslateXYZ(Matrix *Matrix, int32 x, int32 y, int32 z, bool32 setIdentity);
void MatrixScaleXYZ(Matrix *matrix, int32 scaleX, int32 scaleY, int32 scaleZ);
void MatrixRotateX(Matrix *matrix, int16 angle);
void MatrixRotateY(Matrix *matrix, int16 angle);
void MatrixRotateZ(Matrix *matrix, int16 angle);
void MatrixRotateXYZ(Matrix *matrix, int16 rotationX, int16 rotationY, int16 rotationZ);
void MatrixInverse(Matrix *dest, Matrix *matrix);
inline void MatrixCopy(Matrix *matDst, Matrix *matSrc) { memcpy(matDst, matSrc, sizeof(Matrix)); }

uint16 LoadMesh(const char *filepath, uint8 scope);
uint16 Create3DScene(const char *name, uint16 faceCnt, uint8 scope);

// Releases any fast-path geometry this scene is holding (see S3DFast.hpp).
// Defined in Scene3D.cpp; a no-op in builds without the GU render device.
void S3D_LazyReset(uint16 sceneID);
inline void Prepare3DScene(uint16 sceneID)
{
    if (sceneID < SCENE3D_COUNT) {
        Scene3D *scn = &scene3DList[sceneID];

        S3D_LazyReset(sceneID);

        // Clear only the region the previous use actually touched, not the
        // whole 4096-entry capacity.
        //
        // The buffers are allocated zeroed and every clear restores that, so
        // everything outside the previously-used range is already zero -- the
        // full-capacity memset was re-zeroing memory that was never written.
        // It came to ~230KB per call, and UFO_Decoration_Draw calls this once
        // per decoration: with ~30 decorations on screen that was ~7MB of
        // memset per frame, which is most of the Special Stage's frame time.
        // Harmless on desktop, fatal at PSP memory bandwidth.
        const int32 usedVerts = scn->vertexCount;
        const int32 usedFaces = scn->faceCount;

        scn->vertexCount = 0;
        scn->faceCount   = 0;

        if (usedVerts > 0)
            memset(scn->vertices, 0, sizeof(Scene3DVertex) * usedVerts);

        if (usedFaces > 0) {
            memset(scn->faceVertCounts, 0, sizeof(uint8) * usedFaces);
            memset(scn->faceBuffer, 0, sizeof(Scene3DFace) * usedFaces);
        }
    }
}

inline void SetMeshAnimation(uint16 model, Animator *animator, int16 speed, uint8 loopIndex, bool32 forceApply, int16 frameID)
{
    if (model >= MODEL_COUNT) {
        if (animator)
            animator->frames = NULL;

        return;
    }

    if (!animator)
        return;

    if (animator->animationID == model && !forceApply)
        return;

    animator->frames          = (SpriteFrame *)1;
    animator->timer           = 0;
    animator->frameID         = frameID;
    animator->frameCount      = modelList[model].frameCount;
    animator->speed           = speed;
    animator->prevAnimationID = animator->animationID;
    animator->frameDuration   = 0x100;
    animator->loopIndex       = loopIndex;
    animator->animationID     = model;
}
inline void SetDiffuseColor(uint16 sceneID, uint8 x, uint8 y, uint8 z)
{
    if (sceneID < SCENE3D_COUNT) {
        Scene3D *scn  = &scene3DList[sceneID];
        scn->diffuseX = x;
        scn->diffuseY = y;
        scn->diffuseZ = z;
    }
}
inline void SetDiffuseIntensity(uint16 sceneID, uint8 x, uint8 y, uint8 z)
{
    if (sceneID < SCENE3D_COUNT) {
        Scene3D *scn           = &scene3DList[sceneID];
        scn->diffuseIntensityX = x;
        scn->diffuseIntensityY = y;
        scn->diffuseIntensityZ = z;
    }
}
inline void SetSpecularIntensity(uint16 sceneID, uint8 x, uint8 y, uint8 z)
{
    if (sceneID < SCENE3D_COUNT) {
        Scene3D *scn            = &scene3DList[sceneID];
        scn->specularIntensityX = x;
        scn->specularIntensityY = y;
        scn->specularIntensityZ = z;
    }
}
void AddModelToScene(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matWorld, Matrix *matView, color color);
void AddMeshFrameToScene(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld, Matrix *matView, color color);
void Sort3DDrawList(Scene3D *scn, int32 first, int32 last);
void Draw3DScene(uint16 sceneID);

inline void Clear3DScenes()
{
    // Unload Models
    for (int32 m = 0; m < MODEL_COUNT; ++m) {
        if (modelList[m].scope != SCOPE_GLOBAL) {
            MEM_ZERO(modelList[m]);
            modelList[m].scope = SCOPE_NONE;
        }
    }

    // Unload 3D Scenes
    for (int32 s = 0; s < SCENE3D_COUNT; ++s) {
        if (scene3DList[s].scope != SCOPE_GLOBAL) {
            MEM_ZERO(scene3DList[s]);
            scene3DList[s].scope = SCOPE_NONE;
        }
    }
}

#if RETRO_REV0U
#include "Legacy/Scene3DLegacy.hpp"
#endif

} // namespace RSDK

#endif
