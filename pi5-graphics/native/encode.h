#pragma once
#include <stdint.h>
#include "abi.h"
namespace pi5 {
struct Draw {
    uint32_t width, height, pitch, target, tile;
    // Nonzero selects UIF without XOR; otherwise target memory is raster.
    uint32_t tiledRows=0;
    uint32_t coordinateCode, vertexCode, pixelCode;
    uint32_t coordinateUniforms, vertexUniforms, pixelUniforms;
    uint32_t vertexAddress, vertexStride, vertexCount;
    uint32_t clearColor;
    uint32_t varyingScalars=0,nonPerspectiveMask=0,flatMask=0;
    uint32_t vertexScalars=4;
    // Bit 0 coordinate, 1 vertex, 2 pixel: the program uses only rf0-rf31 and may run 4-way threaded.
    uint32_t fourThreadMask=7;
    // Same stage bits: shader starts in V3D's final thread section.
    // Used only for the single-segment RF32-RF63 vertex fallback.
    uint32_t finalThreadMask=0;
    // V3D 7.1 IP revision. 7.1.10 (BCM2712 D0) added draw-index/base-vertex
    // fields and shuffled the first three bytes of the shader-state record.
    // Default 10 preserves Damian's D0 encoding for all non-KMD/native callers.
    uint32_t v3dRevision=10;
    // Raw IEEE floats: top-left x/y, width/height. All zero selects the surface.
    uint32_t viewport[4]={};
    bool loadTarget = false, bgra = false;
    Pi5Pipeline pipeline;
};
struct EncodedCommands {
    uint32_t bytes=0,binStart=0,binEnd=0,renderStart=0,renderEnd=0;
    uint32_t tileBytes=0,stateBytes=0,skippedLoads=0;
};
bool EncodeDraw(const Draw &draw,uint32_t commandAddress,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error);
bool EncodeDrawBatch(const Draw *draws,uint32_t count,uint32_t commandAddress,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error);
bool EncodeClear(const Draw &surface,uint32_t commandAddress,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error);
bool EncodeCopy(const Draw &source,const Draw &destination,uint32_t commandAddress,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error);
}
