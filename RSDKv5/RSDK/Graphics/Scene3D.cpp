#include "RSDK/Core/RetroEngine.hpp"

#include <algorithm> // std::sort, for the face depth sort below

using namespace RSDK;

#if RETRO_RENDERDEVICE_GU
// Splits the Special Stage's cost inside ProcessObjectDrawLists, which
// measures 23-117ms/frame on hardware and drops that stage to 6-16fps.
//
// Two theories have already been wrong: the face sort (replaced with an
// O(n log n) sort -- correct, but changed nothing) and the MAX_FACE_VERTS
// immediate-draw fallback (can't trigger; Scene3D emits 1-4 verts per face).
// The draw queue also reports zero queued faces while the stage visibly
// renders, so the time is somewhere none of the existing counters look.
// Rather than guess a third time, time each stage of the 3D pipeline.
//
// Zero cost when profiling is off -- see GU_ENABLE_PROFILING.
#include <psputils.h>
SceUInt64 gu_s3dMeshUsec = 0;  // per-vertex transform (AddMeshFrameToScene)
SceUInt64 gu_s3dSortUsec = 0;  // depth sort
SceUInt64 gu_s3dDrawUsec = 0;  // face rasterizing
extern int32 gu_profilingEnabled;
#define S3D_TIME_BEGIN(acc) const SceUInt64 acc##_t0 = gu_profilingEnabled ? sceKernelGetSystemTimeWide() : 0
#define S3D_TIME_END(acc)                                                                                                                            \
    if (gu_profilingEnabled)                                                                                                                         \
    acc += sceKernelGetSystemTimeWide() - acc##_t0
#else
#define S3D_TIME_BEGIN(acc) ((void)0)
#define S3D_TIME_END(acc) ((void)0)
#endif

#if RETRO_REV0U
#include "Legacy/Scene3DLegacy.cpp"
#endif

Model RSDK::modelList[MODEL_COUNT];
Scene3D RSDK::scene3DList[SCENE3D_COUNT];

ScanEdge RSDK::scanEdgeBuffer[SCREEN_YSIZE * 2];

enum ModelFlags {
    MODEL_NOFLAGS     = 0,
    MODEL_USENORMALS  = 1 << 0,
    MODEL_USETEXTURES = 1 << 1,
    MODEL_USECOLOURS  = 1 << 2,
};

void RSDK::ProcessScanEdge(int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 ix1 = FROM_FIXED(x1);
    int32 iy1 = FROM_FIXED(y1);
    int32 ix2 = FROM_FIXED(x2);
    int32 iy2 = FROM_FIXED(y2);

    int32 top = FROM_FIXED(y1);
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top = FROM_FIXED(y2);
            ix1 = FROM_FIXED(x2);
            ix2 = FROM_FIXED(x1);
            iy1 = FROM_FIXED(y2);
            iy2 = FROM_FIXED(y1);
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;
            int32 scanPos = TO_FIXED(ix1);
            int32 delta   = TO_FIXED(ix2 - ix1) / (iy2 - iy1);
            if (top < 0) {
                scanPos -= top * delta;
                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                int32 scanX = scanPos >> 16;
                if (scanX < edge->start)
                    edge->start = scanX;
                if (scanX > edge->end)
                    edge->end = scanX;
                scanPos += delta;
                ++edge;
            }
        }
    }
}

void RSDK::ProcessScanEdgeClr(uint32 c1, uint32 c2, int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 iy1 = FROM_FIXED(y1);
    int32 iy2 = FROM_FIXED(y2);
    int32 ix1 = FROM_FIXED(x1);
    int32 ix2 = FROM_FIXED(x2);

    int32 top     = FROM_FIXED(y1);
    uint32 color1 = c1;
    uint32 color2 = c2;
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top    = FROM_FIXED(y2);
            ix1    = FROM_FIXED(x2);
            ix2    = FROM_FIXED(x1);
            iy1    = FROM_FIXED(y2);
            iy2    = FROM_FIXED(y1);
            color1 = c2;
            color2 = c1;
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;

            int32 size   = iy2 - iy1;
            int32 scanX  = TO_FIXED(ix1);
            int32 deltaX = TO_FIXED(ix2 - ix1) / size;

            int32 c1R   = (color1 & 0xFF0000);
            int32 c2R   = (color2 & 0xFF0000);
            int32 scanR = c1R;

            int32 deltaR = 0;
            if (c1R != c2R)
                deltaR = (c2R - c1R) / size;

            int32 c1G   = (color1 & 0x00FF00) << 8;
            int32 c2G   = (color2 & 0x00FF00) << 8;
            int32 scanG = c1G;

            int32 deltaG = 0;
            if (c1G != c2G)
                deltaG = (c2G - c1G) / size;

            int32 c1B   = (color1 & 0x0000FF) << 16;
            int32 c2B   = (color2 & 0x0000FF) << 16;
            int32 scanB = c1B;

            int32 deltaB = 0;
            if (c1B != c2B)
                deltaB = (c2B - c1B) / size;

            if (top < 0) {
                scanX -= top * deltaX;

                scanR -= top * deltaR;
                scanG -= top * deltaG;
                scanB -= top * deltaB;

                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                if (FROM_FIXED(scanX) < edge->start) {
                    edge->start = FROM_FIXED(scanX);

                    edge->startR = scanR;
                    edge->startG = scanG;
                    edge->startB = scanB;
                }

                if (FROM_FIXED(scanX) > edge->end) {
                    edge->end = FROM_FIXED(scanX);

                    edge->endR = scanR;
                    edge->endG = scanG;
                    edge->endB = scanB;
                }

                scanX += deltaX;

                scanR += deltaR;
                scanG += deltaG;
                scanB += deltaB;

                ++edge;
            }
        }
    }
}

void RSDK::SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixMultiply(Matrix *dest, Matrix *matrixA, Matrix *matrixB)
{
    int32 result[4][4];
    memset(result, 0, 4 * 4 * sizeof(int32));

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA        = i / 4;
        uint32 rowB        = i % 4;
        result[rowB][rowA] = (matrixA->values[3][rowA] * matrixB->values[rowB][3] >> 8) + (matrixA->values[2][rowA] * matrixB->values[rowB][2] >> 8)
                             + (matrixA->values[1][rowA] * matrixB->values[rowB][1] >> 8)
                             + (matrixA->values[0][rowA] * matrixB->values[rowB][0] >> 8);
    }

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA              = i / 4;
        uint32 rowB              = i % 4;
        dest->values[rowB][rowA] = result[rowB][rowA];
    }
}
void RSDK::MatrixTranslateXYZ(Matrix *matrix, int32 x, int32 y, int32 z, bool32 setIdentity)
{
    if (setIdentity) {
        matrix->values[0][0] = 0x100;
        matrix->values[1][0] = 0;
        matrix->values[2][0] = 0;
        matrix->values[0][1] = 0;
        matrix->values[1][1] = 0x100;
        matrix->values[2][1] = 0;
        matrix->values[0][2] = 0;
        matrix->values[1][2] = 0;
        matrix->values[2][2] = 0x100;
        matrix->values[3][0] = 0;
        matrix->values[3][1] = 0;
        matrix->values[3][2] = 0;
        matrix->values[3][3] = 0x100;
    }

    matrix->values[0][3] = x >> 8;
    matrix->values[1][3] = y >> 8;
    matrix->values[2][3] = z >> 8;
}
void RSDK::MatrixScaleXYZ(Matrix *matrix, int32 scaleX, int32 scaleY, int32 scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateX(Matrix *matrix, int16 rotationX)
{
    int32 sine   = Sin1024(rotationX & 0x3FF) >> 2;
    int32 cosine = Cos1024(rotationX & 0x3FF) >> 2;

    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = sine;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateY(Matrix *matrix, int16 rotationY)
{
    int32 sine           = Sin1024(rotationY & 0x3FF) >> 2;
    int32 cosine         = Cos1024(rotationY & 0x3FF) >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = sine;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = -sine;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateZ(Matrix *matrix, int16 rotationZ)
{
    int32 sine           = Sin1024(rotationZ & 0x3FF) >> 2;
    int32 cosine         = Cos1024(rotationZ & 0x3FF) >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = -sine;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = sine;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateXYZ(Matrix *matrix, int16 rotationX, int16 rotationY, int16 rotationZ)
{
    int32 sinX = Sin1024(rotationX & 0x3FF) >> 2;
    int32 cosX = Cos1024(rotationX & 0x3FF) >> 2;
    int32 sinY = Sin1024(rotationY & 0x3FF) >> 2;
    int32 cosY = Cos1024(rotationY & 0x3FF) >> 2;
    int32 sinZ = Sin1024(rotationZ & 0x3FF) >> 2;
    int32 cosZ = Cos1024(rotationZ & 0x3FF) >> 2;

    matrix->values[0][0] = (cosZ * cosY >> 8) + (sinZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][1] = -(sinZ * cosX) >> 8;
    matrix->values[0][2] = (sinZ * (cosY * sinX >> 8) >> 8) - (cosZ * sinY >> 8);
    matrix->values[0][3] = 0;
    matrix->values[1][0] = (sinZ * cosY >> 8) - (cosZ * (sinY * sinX >> 8) >> 8);
    matrix->values[1][1] = cosZ * cosX >> 8;
    matrix->values[1][2] = (-(sinZ * sinY) >> 8) - (cosZ * (cosY * sinX >> 8) >> 8);
    matrix->values[1][3] = 0;
    matrix->values[2][0] = sinY * cosX >> 8;
    matrix->values[2][1] = sinX;
    matrix->values[2][2] = cosY * cosX >> 8;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixInverse(Matrix *dest, Matrix *matrix)
{
    double inv[16], det;
    double m[16];
    for (int32 y = 0; y < 4; ++y) {
        for (int32 x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int32 i = 0; i < 0x10; ++i) inv[i] = (int32)((inv[i] * det) * 256);
    for (int32 i = 0; i < 0x10; ++i) dest->values[i / 4][i % 4] = (int32)inv[i];
}

uint16 RSDK::LoadMesh(const char *filename, uint8 scope)
{
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    char fullFilePath[0x100];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Meshes/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(fullFilePath, hash);

    for (int32 i = 0; i < MODEL_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, modelList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < MODEL_COUNT; ++id) {
        if (modelList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= MODEL_COUNT)
        return -1;

    Model *model = &modelList[id];
    FileInfo info;
    InitFileInfo(&info);
    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        uint32 sig = ReadInt32(&info, false);

        if (sig != RSDK_SIGNATURE_MDL) {
            CloseFile(&info);
            return -1;
        }

        model->scope = scope;
        HASH_COPY_MD5(model->hash, hash);

        model->flags         = ReadInt8(&info);
        model->faceVertCount = ReadInt8(&info);

        model->vertCount  = ReadInt16(&info);
        model->frameCount = ReadInt16(&info);

        AllocateStorage((void **)&model->vertices, sizeof(ModelVertex) * model->vertCount * model->frameCount, DATASET_STG, true);
        if (model->flags & MODEL_USETEXTURES)
            AllocateStorage((void **)&model->texCoords, sizeof(TexCoord) * model->vertCount, DATASET_STG, true);
        if (model->flags & MODEL_USECOLOURS)
            AllocateStorage((void **)&model->colors, sizeof(Color) * model->vertCount, DATASET_STG, true);

        if (model->flags & MODEL_USETEXTURES) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->texCoords[v].x = ReadSingle(&info);
                model->texCoords[v].y = ReadSingle(&info);
            }
        }

        if (model->flags & MODEL_USECOLOURS) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->colors[v].color = ReadInt32(&info, false);
            }
        }

        model->indexCount = ReadInt16(&info);
        AllocateStorage((void **)&model->indices, sizeof(uint16) * model->indexCount, DATASET_STG, true);
        for (int32 i = 0; i < model->indexCount; ++i) model->indices[i] = ReadInt16(&info);

        for (int32 f = 0; f < model->frameCount; ++f) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->vertices[(f * model->vertCount) + v].x = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].y = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].z = (int32)(ReadSingle(&info) * 0x100);

                model->vertices[(f * model->vertCount) + v].nx = 0;
                model->vertices[(f * model->vertCount) + v].ny = 0;
                model->vertices[(f * model->vertCount) + v].nz = 0;
                if (model->flags & MODEL_USENORMALS) {
                    model->vertices[(f * model->vertCount) + v].nx = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].ny = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].nz = (int32)(ReadSingle(&info) * 0x10000);
                }
            }
        }

        CloseFile(&info);
        return id;
    }
    return -1;
}
uint16 RSDK::Create3DScene(const char *name, uint16 vertexLimit, uint8 scope)
{
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(name, hash);

    for (int32 i = 0; i < SCENE3D_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, scene3DList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < SCENE3D_COUNT; ++id) {
        if (scene3DList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= SCENE3D_COUNT)
        return -1;

    Scene3D *scene = &scene3DList[id];

    if (vertexLimit > SCENE3D_VERT_COUNT || !vertexLimit)
        vertexLimit = SCENE3D_VERT_COUNT;

    scene->scope = scope;
    HASH_COPY_MD5(scene->hash, hash);
    scene->vertLimit = vertexLimit;
    scene->faceCount = 6;

    scene->projectionX = 8;
    scene->projectionY = 8;
    AllocateStorage((void **)&scene->vertices, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->normals, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceVertCounts, sizeof(uint8) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceBuffer, sizeof(Scene3DFace) * vertexLimit, DATASET_STG, true);

    return id;
}
void RSDK::AddModelToScene(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, color color)
{
    // Counted into the same bucket as AddMeshFrameToScene: both are per-vertex
    // transform work, and only the mesh variant was instrumented before --
    // which left a large slice of the Special Stage's draw-list time invisible.
    S3D_TIME_BEGIN(gu_s3dMeshUsec);
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld) {
            Model *mdl            = &modelList[modelFrames];
            // Matrix elements hoisted into locals.
            //
            // The transform loops below write through `vertex`, which the
            // compiler cannot prove doesn't alias the Matrix these are read
            // from -- so every one of the 12 values was reloaded for EVERY
            // vertex. The Special Stage transforms thousands of vertices per
            // frame, making this the hottest redundant read in the 3D path.
            const int32 mw00 = matWorld->values[0][0], mw01 = matWorld->values[0][1], mw02 = matWorld->values[0][2],
                        mw03 = matWorld->values[0][3];
            const int32 mw10 = matWorld->values[1][0], mw11 = matWorld->values[1][1], mw12 = matWorld->values[1][2],
                        mw13 = matWorld->values[1][3];
            const int32 mw20 = matWorld->values[2][0], mw21 = matWorld->values[2][1], mw22 = matWorld->values[2][2],
                        mw23 = matWorld->values[2][3];
            const int32 mn00 = matNormals ? matNormals->values[0][0] : 0, mn01 = matNormals ? matNormals->values[0][1] : 0,
                        mn02 = matNormals ? matNormals->values[0][2] : 0;
            const int32 mn10 = matNormals ? matNormals->values[1][0] : 0, mn11 = matNormals ? matNormals->values[1][1] : 0,
                        mn12 = matNormals ? matNormals->values[1][2] : 0;
            const int32 mn20 = matNormals ? matNormals->values[2][0] : 0, mn21 = matNormals ? matNormals->values[2][1] : 0,
                        mn22 = matNormals ? matNormals->values[2][2] : 0;

            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[scn->faceCount];
            int32 indCnt          = mdl->indexCount;
            if (scn->vertLimit - vertID >= indCnt) {
                scn->vertexCount += mdl->indexCount;
                scn->drawMode = drawMode;
                scn->faceCount += indCnt / mdl->faceVertCount;

                int32 i = 0;
                int32 f = 0;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                vertex->x = mw03 + (modelVert->z * mw02 >> 8)
                                            + (mw00 * modelVert->x >> 8) + (mw01 * modelVert->y >> 8);
                                vertex->y = mw13 + (modelVert->y * mw11 >> 8)
                                            + (modelVert->z * mw12 >> 8) + (mw10 * modelVert->x >> 8);
                                vertex->z = mw23 + ((modelVert->x * mw20) >> 8)
                                            + ((mw22 * modelVert->z >> 8) + (mw21 * modelVert->y >> 8));

                                vertex->color = color;
                            }
                        }
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = mw03 + (modelVert->z * mw02 >> 8)
                                                + (modelVert->x * mw00 >> 8) + (modelVert->y * mw01 >> 8);
                                    vertex->y = mw13 + (modelVert->y * mw11 >> 8)
                                                + (mw10 * modelVert->x >> 8) + (modelVert->z * mw12 >> 8);
                                    vertex->z = mw23 + (modelVert->x * mw20 >> 8)
                                                + (mw22 * modelVert->z >> 8) + (mw21 * modelVert->y >> 8);

                                    vertex->nx = (modelVert->nz * mn02 >> 8) + (modelVert->nx * mn00 >> 8)
                                                 + (mn01 * modelVert->ny >> 8);
                                    vertex->ny = (modelVert->ny * mn11 >> 8) + (modelVert->nz * mn12 >> 8)
                                                 + (modelVert->nx * mn10 >> 8);
                                    vertex->nz =
                                        ((modelVert->ny * mn21) >> 8)
                                        + ((mn20 * modelVert->nx >> 8) + (modelVert->nz * mn22 >> 8));

                                    vertex->color = color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = mw03 + (modelVert->z * mw02 >> 8)
                                                + (mw00 * modelVert->x >> 8) + (mw01 * modelVert->y >> 8);
                                    vertex->y = mw13 + (modelVert->y * mw11 >> 8)
                                                + (modelVert->z * mw12 >> 8) + (mw10 * modelVert->x >> 8);
                                    vertex->z = mw23 + ((mw22 * modelVert->z) >> 8)
                                                + ((mw20 * modelVert->x >> 8) + (mw21 * modelVert->y >> 8));

                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = mw03 + (mw02 * modelVert->z >> 8)
                                                + (modelVert->y * mw01 >> 8) + (mw00 * modelVert->x >> 8);
                                    vertex->y = mw13 + (mw12 * modelVert->z >> 8)
                                                + (modelVert->y * mw11 >> 8) + (mw10 * modelVert->x >> 8);
                                    vertex->z = mw23 + (modelVert->x * mw20 >> 8)
                                                + (modelVert->y * mw21 >> 8) + (mw22 * modelVert->z >> 8);

                                    vertex->nx = (mn00 * modelVert->nx >> 8) + (modelVert->ny * mn01 >> 8)
                                                 + (mn02 * modelVert->nz >> 8);
                                    vertex->ny = (mn10 * modelVert->nx >> 8) + (modelVert->ny * mn11 >> 8)
                                                 + (mn12 * modelVert->nz >> 8);
                                    vertex->nz =
                                        ((mn22 * modelVert->nz) >> 8)
                                        + ((modelVert->ny * mn21 >> 8) + (mn20 * modelVert->nx >> 8));

                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = mw03 + (mw00 * modelVert->x >> 8)
                                                + (modelVert->y * mw01 >> 8) + (modelVert->z * mw02 >> 8);
                                    vertex->y = mw13 + (modelVert->z * mw12 >> 8)
                                                + (mw10 * modelVert->x >> 8) + (modelVert->y * mw11 >> 8);
                                    vertex->z = mw23 + (mw22 * modelVert->z >> 8)
                                                + (modelVert->y * mw21 >> 8) + (modelVert->x * mw20 >> 8);

                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
    S3D_TIME_END(gu_s3dMeshUsec);
}
void RSDK::AddMeshFrameToScene(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld, Matrix *matNormals,
                               color color)
{
    S3D_TIME_BEGIN(gu_s3dMeshUsec);
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld && animator) {
            Model *mdl            = &modelList[modelFrames];
            // Matrix elements hoisted into locals.
            //
            // The transform loops below write through `vertex`, which the
            // compiler cannot prove doesn't alias the Matrix these are read
            // from -- so every one of the 12 values was reloaded for EVERY
            // vertex. The Special Stage transforms thousands of vertices per
            // frame, making this the hottest redundant read in the 3D path.
            const int32 mw00 = matWorld->values[0][0], mw01 = matWorld->values[0][1], mw02 = matWorld->values[0][2],
                        mw03 = matWorld->values[0][3];
            const int32 mw10 = matWorld->values[1][0], mw11 = matWorld->values[1][1], mw12 = matWorld->values[1][2],
                        mw13 = matWorld->values[1][3];
            const int32 mw20 = matWorld->values[2][0], mw21 = matWorld->values[2][1], mw22 = matWorld->values[2][2],
                        mw23 = matWorld->values[2][3];
            const int32 mn00 = matNormals ? matNormals->values[0][0] : 0, mn01 = matNormals ? matNormals->values[0][1] : 0,
                        mn02 = matNormals ? matNormals->values[0][2] : 0;
            const int32 mn10 = matNormals ? matNormals->values[1][0] : 0, mn11 = matNormals ? matNormals->values[1][1] : 0,
                        mn12 = matNormals ? matNormals->values[1][2] : 0;
            const int32 mn20 = matNormals ? matNormals->values[2][0] : 0, mn21 = matNormals ? matNormals->values[2][1] : 0,
                        mn22 = matNormals ? matNormals->values[2][2] : 0;

            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[scn->faceCount];
            int32 indCnt          = mdl->indexCount;
            if (scn->vertLimit - vertID >= indCnt) {
                scn->vertexCount += mdl->indexCount;
                scn->drawMode = drawMode;
                scn->faceCount += indCnt / mdl->faceVertCount;

                int32 nextFrame = animator->frameID + 1;
                if (nextFrame >= animator->frameCount)
                    nextFrame = animator->loopIndex;
                int32 frameOffset     = animator->frameID * mdl->vertCount;
                int32 nextFrameOffset = nextFrame * mdl->vertCount;

                int32 i           = 0;
                int32 f           = 0;
                int32 interpolate = animator->timer;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                i++;
                                Scene3DVertex *vertex = &scn->vertices[vertID++];
                                vertex->x             = mw03 + (z * mw02 >> 8) + (mw00 * x >> 8)
                                            + (mw01 * y >> 8);
                                vertex->y = mw13 + (y * mw11 >> 8) + (z * mw12 >> 8)
                                            + (mw10 * x >> 8);
                                vertex->z = mw23 + ((x * mw20) >> 8)
                                            + ((mw22 * z >> 8) + (mw21 * y >> 8));
                                vertex->color = color;
                            }
                        }
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);
                                    i++;

                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = mw03 + (z * mw02 >> 8) + (x * mw00 >> 8)
                                                + (y * mw01 >> 8);
                                    vertex->y = mw13 + (y * mw11 >> 8) + (mw10 * x >> 8)
                                                + (z * mw12 >> 8);
                                    vertex->z = mw23 + (x * mw20 >> 8) + (mw22 * z >> 8)
                                                + (mw21 * y >> 8);
                                    vertex->nx = (nz * mn02 >> 8) + (nx * mn00 >> 8)
                                                 + (mn01 * ny >> 8);
                                    vertex->ny = (ny * mn11 >> 8) + (nz * mn12 >> 8)
                                                 + (nx * mn10 >> 8);
                                    vertex->nz = ((ny * mn21) >> 8)
                                                 + ((mn20 * nx >> 8) + (nz * mn22 >> 8));
                                    vertex->color = color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    i++;
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = mw03 + (z * mw02 >> 8) + (mw00 * x >> 8)
                                                + (mw01 * y >> 8);
                                    vertex->y = mw13 + (y * mw11 >> 8) + (z * mw12 >> 8)
                                                + (mw10 * x >> 8);
                                    vertex->z = mw23 + ((mw22 * z) >> 8)
                                                + ((mw20 * x >> 8) + (mw21 * y >> 8));
                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);

                                    Color *modelColor     = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = mw03 + (mw02 * z >> 8) + (y * mw01 >> 8)
                                                + (mw00 * x >> 8);
                                    vertex->y = mw13 + (mw12 * z >> 8) + (y * mw11 >> 8)
                                                + (mw10 * x >> 8);
                                    vertex->z = mw23 + (x * mw20 >> 8) + (y * mw21 >> 8)
                                                + (mw22 * z >> 8);
                                    vertex->nx = (mn00 * nx >> 8) + (ny * mn01 >> 8)
                                                 + (mn02 * nz >> 8);
                                    vertex->ny = (mn10 * nx >> 8) + (ny * mn11 >> 8)
                                                 + (mn12 * nz >> 8);
                                    vertex->nz = ((mn22 * nz) >> 8)
                                                 + ((ny * mn21 >> 8) + (mn20 * nx >> 8));
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    Color *modelColor          = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex      = &scn->vertices[vertID++];
                                    vertex->x = mw03 + (mw00 * x >> 8) + (y * mw01 >> 8)
                                                + (z * mw02 >> 8);
                                    vertex->y = mw13 + (z * mw12 >> 8) + (mw10 * x >> 8)
                                                + (y * mw11 >> 8);
                                    vertex->z = mw23 + (mw22 * z >> 8) + (y * mw21 >> 8)
                                                + (x * mw20 >> 8);
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
    S3D_TIME_END(gu_s3dMeshUsec);
}

void RSDK::Draw3DScene(uint16 sceneID)
{
    if (sceneID < SCENE3D_COUNT) {
        Entity *entity = sceneInfo.entity;
        Scene3D *scn   = &scene3DList[sceneID];

        // Scene/screen constants hoisted out of the per-vertex loops below.
        //
        // Each draw mode reads these through the `scn` and `currentScreen`
        // pointers inside loops that write to vertPos[]/vertClrs[]. The
        // compiler cannot prove those writes don't alias the structs, so all
        // of them were reloaded for EVERY vertex -- 13 dependent loads per
        // vertex in the shaded modes, across thousands of vertices a frame.
        const int32 s3dProjX = scn->projectionX, s3dProjY = scn->projectionY;
        const int32 s3dDiffX = scn->diffuseX, s3dDiffY = scn->diffuseY, s3dDiffZ = scn->diffuseZ;
        const int32 s3dDiffIX = scn->diffuseIntensityX, s3dDiffIY = scn->diffuseIntensityY, s3dDiffIZ = scn->diffuseIntensityZ;
        const int32 s3dSpecIX = scn->specularIntensityX, s3dSpecIY = scn->specularIntensityY, s3dSpecIZ = scn->specularIntensityZ;
        const int32 s3dCenterX = currentScreen->center.x, s3dCenterY = currentScreen->center.y;
        const int32 s3dPosX = currentScreen->position.x, s3dPosY = currentScreen->position.y;


        // Setup face buffer.
        // Each face's depth is an average of the depth of its vertices.
        Scene3DVertex *vertices = scn->vertices;
        Scene3DFace *faceBuffer = scn->faceBuffer;
        uint8 *faceVertCounts = scn->faceVertCounts;

        int32 vertIndex = 0;
        for (int32 i = 0; i < scn->faceCount; ++i) {
            switch (*faceVertCounts) {
                default:
                case 1:
                    faceBuffer->depth = vertices[0].z;
                    vertices += *faceVertCounts;
                    break;

                case 2:
                    faceBuffer->depth = vertices[0].z >> 1;
                    faceBuffer->depth += vertices[1].z >> 1;
                    vertices += 2;
                    break;

                case 3:
                    faceBuffer->depth = vertices[0].z >> 1;
                    faceBuffer->depth = (faceBuffer->depth + (vertices[1].z >> 1)) >> 1;
                    faceBuffer->depth += vertices[2].z >> 1;
                    vertices += 3;
                    break;

                case 4:
                    faceBuffer->depth = vertices[0].z >> 2;
                    faceBuffer->depth += vertices[1].z >> 2;
                    faceBuffer->depth += vertices[2].z >> 2;
                    faceBuffer->depth += vertices[3].z >> 2;
                    vertices += 4;
                    break;
            }

            faceBuffer->index = vertIndex;
            vertIndex += *faceVertCounts;

            ++faceBuffer;
            ++faceVertCounts;
        }

        // Sort the face buffer back-to-front, so faces don't overlap each other
        // incorrectly when rendered.
        //
        // This was an insertion sort -- O(n^2). Measured on PSP hardware, the
        // Special Stage spent 26-117ms per frame inside ProcessObjectDrawLists,
        // dropping it to 6-16fps, and the quadratic growth is exactly why the
        // cost swung so wildly with how much geometry was on screen.
        //
        // std::sort is O(n log n). It isn't stable, but it doesn't need to be
        // here: `index` is the face's vertex offset, which increases
        // monotonically with face order, so tie-breaking on it reproduces the
        // insertion sort's ordering for equal depths exactly -- important
        // because coplanar faces reordering frame to frame would shimmer.
        S3D_TIME_BEGIN(gu_s3dSortUsec);
        std::sort(scn->faceBuffer, scn->faceBuffer + scn->faceCount, [](const Scene3DFace &a, const Scene3DFace &b) {
            if (a.depth != b.depth)
                return a.depth > b.depth; // farthest first
            return a.index < b.index;     // preserve original order within a depth
        });
        S3D_TIME_END(gu_s3dSortUsec);

        S3D_TIME_BEGIN(gu_s3dDrawUsec);

        // Finally, display the faces.

        uint8 *vertCnt = scn->faceVertCounts;
        Vector2 vertPos[4];
        uint32 vertClrs[4];

        switch (scn->drawMode) {
            default: break;

            case S3D_WIREFRAME:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, drawVert[0].color,
                                 entity->alpha, entity->inkEffect, false);
                    }
                    DrawLine(drawVert[0].x << 8, drawVert[0].y << 8, drawVert[*vertCnt - 1].x << 8, drawVert[*vertCnt - 1].y << 8, drawVert[0].color,
                             entity->alpha, entity->inkEffect, false);
                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (s3dPosX << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (s3dPosY << 16);
                    }
                    DrawFace(vertPos, *vertCnt, (drawVert->color >> 16) & 0xFF, (drawVert->color >> 8) & 0xFF, (drawVert->color >> 0) & 0xFF,
                             entity->alpha, entity->inkEffect);
                    vertCnt++;
                }
                break;

            // Might have been reserved for textures?
            // not sure about this, just a guess based on tex coords existing in the model format spec
            case S3D_UNUSED_1: break;
            case S3D_UNUSED_2: break;

            case S3D_WIREFRAME_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny1 = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny1 += drawVert[v].ny;
                    }

                    int32 normal    = ny1 / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> s3dSpecIX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                    specular = normalVal >> 6 >> s3dSpecIY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                    specular = normalVal >> 6 >> s3dSpecIZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    for (int32 v = 0; v < vertCount - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, color, entity->alpha,
                                 entity->inkEffect, false);
                    }
                    DrawLine(drawVert[vertCount - 1].x << 8, drawVert[vertCount - 1].y << 8, drawVert[0].x << 8, drawVert[0].y << 8, color,
                             entity->alpha, entity->inkEffect, false);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny += drawVert[v].ny;
                        vertPos[v].x = (drawVert[v].x << 8) - (s3dPosX << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (s3dPosY << 16);
                    }

                    int32 normal    = ny / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> s3dSpecIX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                    specular = normalVal >> 6 >> s3dSpecIY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                    specular = normalVal >> 6 >> s3dSpecIZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    DrawFace(vertPos, *vertCnt, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    for (int32 v = 0; v < vertCount; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (s3dPosX << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (s3dPosY << 16);

                        int32 normal    = drawVert[v].ny;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = (normalVal >> 6) >> s3dSpecIX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                        specular = (normalVal >> 6) >> s3dSpecIY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                        specular = (normalVal >> 6) >> s3dSpecIZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        vertClrs[v] = (r << 16) | (g << 8) | (b << 0);
                    }

                    DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];

                    int32 v = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = s3dCenterX + (drawVert[v].x << s3dProjX) / vertZ;
                            vertPos[v].y = s3dCenterY - (drawVert[v].y << s3dProjY) / vertZ;
                        }
                    }

                    if (v < 0xFF) {
                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, drawVert[0].color, entity->alpha,
                                     entity->inkEffect, true);
                        }
                        DrawLine(vertPos[0].x, vertPos[0].y, vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, drawVert[0].color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (s3dCenterX << 16) + ((drawVert[v].x << s3dProjX) / vertZ << 16);
                            vertPos[v].y = (s3dCenterY << 16) - ((drawVert[v].y << s3dProjY) / vertZ << 16);
                        }
                    }

                    if (v < 0xFF) {
                        DrawFace(vertPos, *vertCnt, (drawVert[0].color >> 16) & 0xFF, (drawVert[0].color >> 8) & 0xFF,
                                 (drawVert[0].color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }
                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SHADED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v   = 0;
                    int32 ny1 = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = s3dCenterX + (drawVert[v].x << s3dProjX) / vertZ;
                            vertPos[v].y = s3dCenterY - (drawVert[v].y << s3dProjY) / vertZ;
                            ny1 += drawVert[v].ny;
                        }
                    }

                    if (v < 0xFF) {
                        int32 normal    = ny1 / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> s3dSpecIX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                        specular = normalVal >> 6 >> s3dSpecIY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                        specular = normalVal >> 6 >> s3dSpecIZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, color, entity->alpha, entity->inkEffect,
                                     true);
                        }
                        DrawLine(vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, vertPos[0].x, vertPos[0].y, color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v  = 0;
                    int32 ny = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (s3dCenterX << 16) + ((drawVert[v].x << s3dProjX) / vertZ << 16);
                            vertPos[v].y = (s3dCenterY << 16) - ((drawVert[v].y << s3dProjY) / vertZ << 16);
                            ny += drawVert[v].ny;
                        }
                    }

                    if (v < 0xFF) {
                        int32 normal    = ny / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> s3dSpecIX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                        specular = normalVal >> 6 >> s3dSpecIY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                        specular = normalVal >> 6 >> s3dSpecIZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        drawVert = &scn->vertices[scn->faceBuffer[f].index];
                        DrawFace(vertPos, *vertCnt, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (s3dCenterX << 16) + ((drawVert[v].x << s3dProjX) / vertZ << 16);
                            vertPos[v].y = (s3dCenterY << 16) - ((drawVert[v].y << s3dProjY) / vertZ << 16);

                            int32 normal    = drawVert[v].ny;
                            int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                            int32 specular = normalVal >> 6 >> s3dSpecIX;
                            specular       = CLAMP(specular, 0x00, 0xFF);
                            int32 r =
                                specular + ((int32)((drawVert[v].color >> 16) & 0xFF) * ((normal >> 10) + s3dDiffX) >> s3dDiffIX);

                            specular = normalVal >> 6 >> s3dSpecIY;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 g =
                                specular + ((int32)((drawVert[v].color >> 8) & 0xFF) * ((normal >> 10) + s3dDiffY) >> s3dDiffIY);

                            specular = normalVal >> 6 >> s3dSpecIZ;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 b =
                                specular + ((int32)((drawVert[v].color >> 0) & 0xFF) * ((normal >> 10) + s3dDiffZ) >> s3dDiffIZ);

                            r = CLAMP(r, 0x00, 0xFF);
                            g = CLAMP(g, 0x00, 0xFF);
                            b = CLAMP(b, 0x00, 0xFF);

                            vertClrs[v] = (r << 16) | (g << 8) | (b << 0);
                        }
                    }

                    if (v < 0xFF) {
                        drawVert = &scn->vertices[scn->faceBuffer[f].index];
                        DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);
                    }

                    vertCnt++;
                }
                break;
        }
        S3D_TIME_END(gu_s3dDrawUsec);
    }
}
