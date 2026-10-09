#include "encode.h"
#include <string.h>

namespace pi5 {
namespace {
// One 32-bit color attachment, no MSAA or depth storage: the V3D 7.1
// tile buffer accommodates a full 64x64 tile, matching Mesa's selection.
constexpr uint32_t TileWidth=64,TileHeight=64;
struct Writer {
    uint8_t *bytes;
    uint32_t capacity,used=0,packet=0,length=0;
    bool valid=true;
    Writer(void *buffer,uint32_t size):bytes(static_cast<uint8_t*>(buffer)),capacity(size){}
    uint32_t Address(uint32_t base) const {return base+used;}
    void Align(unsigned alignment){
        uint32_t next=(used+alignment-1)&~(alignment-1);
        if(next>capacity){valid=false;return;}
        memset(bytes+used,0,next-used);used=next;
    }
    void Begin(unsigned opcode,unsigned count){
        if(!valid||!count||count>capacity-used){valid=false;return;}
        packet=used;length=count;memset(bytes+packet,0,count);used+=count;bytes[packet]=static_cast<uint8_t>(opcode);
    }
    void Bits(unsigned first,unsigned count,uint32_t value){
        if(!valid||!count||count>32||first+count>length*8||(count<32&&value>=(1u<<count))){valid=false;return;}
        for(unsigned bit=0;bit<count;++bit)if(value&(1u<<bit))bytes[packet+(first+bit)/8]|=uint8_t(1u<<((first+bit)%8));
    }
    void IntegerFloat(unsigned first,uint32_t value,bool negative=false){
        uint32_t bits=0;
        if(value){unsigned top=0;for(uint32_t n=value;n>1;n>>=1)++top;bits=((top+127)<<23)|((value<<(23-top))&0x7fffff);}
        Bits(first,32,bits|(negative?0x80000000u:0));
    }
};
void TileBuffer(Writer &w,const Draw &d,bool load,bool empty=false) {
    w.Begin(load ? 30 : 29,13);
    if (empty) {w.Bits(8,4,8);return;}
    w.Bits(12,3,d.tiledRows?4:0);w.Bits(20,6,27);w.Bits(28,1,d.bgra);w.Bits(36,20,d.tiledRows?d.tiledRows:d.pitch);w.Bits(72,32,d.target);
}
void ClipRect(const Draw&d,uint32_t rect[4]){
    rect[0]=rect[1]=0;rect[2]=d.width;rect[3]=d.height;
    if(d.viewport[0]|d.viewport[1]|d.viewport[2]|d.viewport[3]){uint32_t fixed[4]={};(void)Pi5ViewportRect(d.viewport,d.width,d.height,fixed);
        // V3D clips against a guard band. Restrict pixel centres to the D3D
        // viewport too, including fractional edges and top/left inclusion.
        for(unsigned axis=0;axis<2;++axis){rect[axis]=(fixed[axis]+32767)>>16;rect[axis+2]=(fixed[axis]+fixed[axis+2]+32767)>>16;}
    }
    if(d.pipeline.Flags&8)for(unsigned axis=0;axis<2;++axis){if(rect[axis]<d.pipeline.Scissor[axis])rect[axis]=d.pipeline.Scissor[axis];if(rect[axis+2]>d.pipeline.Scissor[axis+2])rect[axis+2]=d.pipeline.Scissor[axis+2];}
    if(rect[2]<=rect[0]||rect[3]<=rect[1])rect[2]=rect[0],rect[3]=rect[1];
}
#define Need(good,message) do {if(!(good)){error=message;out={};return false;}}while(0)
}
bool EncodeClear(const Draw &d,uint32_t base,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error) {
    out={};error=nullptr;
    Need(buffer&&capacity&&capacity<=256*1024,"invalid command storage");
    {
        Need(d.width && d.height && d.width<=4096 && d.height<=4096 &&
             d.pitch>=d.width*4 && d.pitch<=65536 && !(d.pitch&3),"invalid clear surface dimensions");
        Need(base && !(base&4095) && base<=0xfffc0000 && d.target && !(d.target&63),"invalid clear buffer alignment");
        Need(!d.tiledRows||(d.tiledRows==(d.height+7)/8&&!(d.target&4095)),"invalid tiled clear layout");
        Writer w(buffer,capacity);out.renderStart=base;
        w.Begin(121,9);w.Bits(16,16,d.width);w.Bits(32,16,d.height);w.Bits(52,1,1);w.Bits(54,1,1);w.Bits(60,3,3);w.Bits(63,3,3);
        w.Begin(121,9);w.Bits(8,3,2);w.Bits(26,7,31);w.Bits(35,5,8);w.Bits(40,32,d.clearColor);
        w.Begin(121,9);w.Bits(8,4,1);
        for(unsigned i=0;i<2;++i){w.Begin(124,4);w.Begin(26,1);TileBuffer(w,d,false,true);if(!i)w.Begin(25,1);w.Begin(27,1);}
        w.Begin(19,1);
        for(unsigned y=0;y<(d.height+TileHeight-1)/TileHeight;++y)for(unsigned x=0;x<(d.width+TileWidth-1)/TileWidth;++x){
            w.Begin(124,4);w.Bits(8,12,x);w.Bits(20,12,y);
            w.Begin(26,1);TileBuffer(w,d,false);w.Begin(25,1);w.Begin(27,1);
        }
        w.Begin(13,1);out.renderEnd=w.Address(base);Need(w.valid,"command storage or field overflow");out.bytes=w.used;return true;
    }
}
bool EncodeDraw(const Draw &d,uint32_t base,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error) {
    return EncodeDrawBatch(&d,1,base,buffer,capacity,out,error);
}
bool EncodeCopy(const Draw &source,const Draw &d,uint32_t base,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error) {
    out={};error=nullptr;
    Need(buffer&&capacity&&capacity<=256*1024,"invalid copy command storage");
    Need(d.width&&d.height&&d.width<=4096&&d.height<=4096&&source.width==d.width&&source.height==d.height,"invalid copy dimensions");
    const Draw*surfaces[]={&source,&d};for(const Draw*surface:surfaces)Need(surface->pitch>=surface->width*4&&surface->pitch<=65536&&!(surface->pitch&3)&&surface->target&&!(surface->target&63),"invalid copy surface");
    for(const Draw*surface:surfaces)Need(!surface->tiledRows||(surface->tiledRows==(surface->height+7)/8&&!(surface->target&4095)),"invalid tiled copy layout");
    Need(base&&!(base&4095)&&base<=0xfffc0000,"invalid copy command address");
    Writer w(buffer,capacity);uint32_t generic=w.Address(base);
    TileBuffer(w,source,true);w.Begin(26,1);TileBuffer(w,d,false);w.Begin(27,1);w.Begin(18,1);
    w.Align(64);out.renderStart=w.Address(base);
    w.Begin(121,9);w.Bits(16,16,d.width);w.Bits(32,16,d.height);w.Bits(52,1,1);w.Bits(54,1,1);w.Bits(60,3,3);w.Bits(63,3,3);
    w.Begin(121,9);w.Bits(8,3,2);w.Bits(26,7,31);w.Bits(35,5,8);
    w.Begin(121,9);w.Bits(8,4,1);
    for(unsigned i=0;i<2;++i){w.Begin(124,4);w.Begin(26,1);TileBuffer(w,d,false,true);if(!i)w.Begin(25,1);w.Begin(27,1);}
    w.Begin(19,1);
    for(unsigned y=0;y<(d.height+TileHeight-1)/TileHeight;++y)for(unsigned x=0;x<(d.width+TileWidth-1)/TileWidth;++x){
        w.Begin(124,4);w.Bits(8,12,x);w.Bits(20,12,y);w.Begin(17,5);w.Bits(8,32,generic);
    }
    w.Begin(13,1);out.renderEnd=w.Address(base);Need(w.valid,"copy command storage overflow");out.bytes=w.used;return true;
}
bool EncodeDrawBatch(const Draw *draws,uint32_t count,uint32_t base,void *buffer,uint32_t capacity,EncodedCommands &out,const char *&error) {
    out={};error=nullptr;
    Need(draws&&count&&count<=PI5_MAX_BATCH_DRAWS,"invalid draw batch count");
    Need(buffer&&capacity&&capacity<=256*1024,"invalid command storage");
    for(uint32_t drawIndex=0;drawIndex<count;++drawIndex){const auto&d=draws[drawIndex];
        Need(d.width && d.height && d.width <= 4096 && d.height <= 4096 &&
             d.pitch >= d.width * 4 && d.pitch <= 65536 && !(d.pitch & 3),"invalid render surface dimensions");
        Need(d.vertexScalars && d.vertexScalars<=PI5_MAX_VERTEX_SCALARS && d.vertexCount && d.vertexCount <= 4096 && !(d.vertexCount % 3) && d.vertexStride >= d.vertexScalars*4 &&
             d.vertexStride <= 4096 && !(d.vertexStride & 3),"invalid triangle vertex range");
        Need(Pi5ValidVaryingMasks(d.varyingScalars,d.nonPerspectiveMask,d.flatMask),"invalid varying count or interpolation mask");
        bool viewportSet=(d.viewport[0]|d.viewport[1]|d.viewport[2]|d.viewport[3])!=0;
        uint32_t viewportFixed[4]={};
        if(viewportSet)Need(Pi5ViewportRect(d.viewport,d.width,d.height,viewportFixed),"invalid viewport rectangle");
        const auto&state=d.pipeline;
        Need(!(state.Flags&~63u)&&!((state.Flags&1)&&(state.Flags&PI5_PIPELINE_SHADER_BLEND))&&state.CullMode>=1&&state.CullMode<=3&&state.ColorMask<=15&&!state.Reserved,"invalid pipeline state");
        Need(Pi5ValidCoverage(state,d.width,d.height),"invalid opaque coverage rectangle");
        for(unsigned i=0;i<6;++i)Need(state.Blend[i]<=(i%3==2?4u:14u),"invalid blend factor or operation");
        for(auto factor:state.Constant)Need(factor<=0x3c00,"invalid blend constant");
        if(state.Flags&8)Need(state.Scissor[0]<state.Scissor[2]&&state.Scissor[1]<state.Scissor[3]&&state.Scissor[2]<=d.width&&state.Scissor[3]<=d.height,"invalid scissor rectangle");
        else for(auto edge:state.Scissor)Need(!edge,"unexpected scissor rectangle");
        Need(base && !(base & 4095) && base <= 0xffff0000 && d.tile && !(d.tile & 4095) &&
             d.target && !(d.target & 63) && d.vertexAddress && !(d.vertexAddress & 3),"invalid GPU buffer alignment");
        Need(!d.tiledRows||(d.tiledRows==(d.height+7)/8&&!(d.target&4095)),"invalid tiled render layout");
        Need(d.coordinateCode && d.vertexCode && d.pixelCode &&
             !((d.coordinateCode | d.vertexCode | d.pixelCode) & 7) &&
             d.coordinateUniforms && d.vertexUniforms && d.pixelUniforms &&
             !((d.coordinateUniforms | d.vertexUniforms | d.pixelUniforms) & 3),"invalid shader addresses");
        const auto&first=draws[0];
        Need(d.width==first.width&&d.height==first.height&&d.pitch==first.pitch&&d.tiledRows==first.tiledRows&&d.target==first.target&&d.tile==first.tile&&d.bgra==first.bgra&&d.loadTarget==first.loadTarget&&d.clearColor==first.clearColor&&d.v3dRevision==first.v3dRevision,"incompatible batch surfaces");
    }
    {const auto&d=draws[0];
        uint32_t tilesX = (d.width + TileWidth-1) / TileWidth, tilesY = (d.height + TileHeight-1) / TileHeight;
        // V3D allows at most 256 supertiles, with each dimension below 256.
        // Group tiles as Mesa does instead of describing every tile as its
        // own supertile on large render targets.
        uint32_t superWidth=1,superHeight=1,superX=tilesX,superY=tilesY;
        while(superX>=256||superY>=256||superX*superY>256){
            if(superWidth<superHeight)++superWidth;else ++superHeight;
            superX=(tilesX+superWidth-1)/superWidth;superY=(tilesY+superHeight-1)/superHeight;
        }
        out.tileBytes = ((tilesX * tilesY * 64 + 4095) & ~4095u) + 8192 + 1024 * 1024;
        out.stateBytes = (tilesX * tilesY * 256 + 4095) & ~4095u;
        Writer w(buffer,capacity);
        bool coverage=d.loadTarget&&(d.pipeline.Flags&PI5_PIPELINE_COVERAGE);
        auto Generic=[&](bool load){uint32_t start=w.Address(base);
            w.Begin(125,1);if(load)TileBuffer(w,d,true);
            w.Begin(26,1);w.Begin(56,2);w.Bits(8,8,2);
            w.Begin(54,5);w.Begin(21,2);TileBuffer(w,d,false);
            // A skipped load always starts from a defined tile. Clear after
            // every store, including loaded tiles, just as a clear-on-load job
            // does. An invalid user-mode hint can never reveal prior tile data.
            if(!d.loadTarget||coverage)w.Begin(25,1);
            w.Begin(27,1);w.Begin(18,1);return start;
        };
        uint32_t generic=Generic(d.loadTarget),genericEnd=w.Address(base);
        uint32_t opaque=coverage?Generic(false):generic,opaqueEnd=coverage?w.Address(base):genericEnd;
        uint32_t records[PI5_MAX_BATCH_DRAWS]={};
        for(uint32_t n=0;n<count;++n){const auto&item=draws[n];
        w.Align(32);records[n] = w.Address(base);
        w.Begin(0,32);w.Bits(1,1,1);
        if(item.v3dRevision>=10){
            // V3D 7.1.10 / BCM2712 D0: GL_SHADER_STATE_RECORD_DRAW_INDEX.
            // Preserve Damian's original bit layout byte-for-byte.
            w.Bits(13,1,1);                              // turn_off_early_z_test
            w.Bits(21,1,1);                              // disable_implicit_point_line_varyings
            w.Bits(15,1,item.varyingScalars!=0);          // real pixel-centre W
        }else{
            // V3D 7.1.6 / BCM2712 C1: original GL_SHADER_STATE_RECORD.
            // 7.1.10 inserted draw-index/base-vertex fields and moved these
            // same semantics within the first three bytes.
            w.Bits(9,1,1);                               // turn_off_early_z_test
            w.Bits(18,1,1);                              // disable_implicit_point_line_varyings
            w.Bits(12,1,item.varyingScalars!=0);          // real pixel-centre W
        }
        w.Bits(24,8,item.varyingScalars);
        w.Bits(32,4,((item.vertexScalars>6?item.vertexScalars:6)+7)/8);w.Bits(48,4,((item.vertexScalars>4+item.varyingScalars?item.vertexScalars:4+item.varyingScalars)+7)/8);
        w.Bits(64,32,item.pixelCode | ((item.fourThreadMask>>2)&1));w.Bits(96,32,item.pixelUniforms);
        w.Bits(128,32,item.vertexCode | ((item.fourThreadMask>>1)&1));w.Bits(160,32,item.vertexUniforms);
        w.Bits(192,32,item.coordinateCode | (item.fourThreadMask&1));w.Bits(224,32,item.coordinateUniforms);
        uint32_t attributes=(item.vertexScalars+3)/4;
        for(uint32_t i=0;i<attributes;++i){uint32_t values=item.vertexScalars-i*4;if(values>4)values=4;
            w.Begin(0,16);w.Bits(0,32,item.vertexAddress+i*16);w.Bits(32,2,values&3);w.Bits(34,3,6);w.Bits(39,1,1);
            w.Bits(40,4,values);w.Bits(44,4,values);w.Bits(64,32,item.vertexStride);w.Bits(96,32,item.vertexCount - 1);
        }
        }
        w.Align(64);out.binStart = w.Address(base);
        w.Begin(119,2);
        w.Begin(120,9);w.Bits(12,2,1);w.Bits(16,3,3);w.Bits(19,3,3);w.Bits(40,16,d.width-1);w.Bits(56,16,d.height-1);
        w.Begin(19,1);w.Begin(92,5);w.Begin(6,1);
        for(uint32_t n=0;n<count;++n){const auto&item=draws[n];
        bool viewportSet=(item.viewport[0]|item.viewport[1]|item.viewport[2]|item.viewport[3])!=0;
        uint32_t viewportFixed[4]={};if(viewportSet)(void)Pi5ViewportRect(item.viewport,item.width,item.height,viewportFixed);
        w.Begin(107,9);
        uint32_t clip[4];ClipRect(item,clip);w.Bits(8,16,clip[0]);w.Bits(24,16,clip[1]);w.Bits(40,16,clip[2]-clip[0]);w.Bits(56,16,clip[3]-clip[1]);
        w.Begin(108,9);
        w.Bits(8,22,viewportSet?(viewportFixed[0]+viewportFixed[2]/2)>>8:item.width*128);
        w.Bits(40,22,viewportSet?(viewportFixed[1]+viewportFixed[3]/2)>>8:item.height*128);
        w.Begin(110,9);
        // Clipped vertices are projected by the fixed-function clipper. Its
        // Y scale must match the negative scale emitted by the coordinate and
        // vertex shaders for Direct3D's Y-down viewport. Otherwise triangles
        // crossing the guard band acquire reflected screen-space vertices.
        if(viewportSet){w.Bits(8,32,item.viewport[2]+(5u<<23));w.Bits(40,32,(item.viewport[3]+(5u<<23))|0x80000000u);}
        else{w.IntegerFloat(8,item.width*32);w.IntegerFloat(40,item.height*32,true);}
        w.Begin(111,9);w.IntegerFloat(8,1);w.IntegerFloat(40,0);
        w.Begin(109,9);w.IntegerFloat(8,0);w.IntegerFloat(40,1);
        // D3D front-face winding is defined in a Y-down render target. The
        // coordinate shader reverses Y, so V3D's winding selector is inverted.
        // Direct3D flat interpolation uses the first vertex of each primitive.
        w.Begin(96,4);w.Bits(8,1,item.pipeline.CullMode!=2);w.Bits(9,1,item.pipeline.CullMode!=3);w.Bits(10,1,(item.pipeline.Flags&2)!=0);w.Bits(20,3,7);w.Bits(27,1,(item.pipeline.Flags&1)!=0);w.Bits(29,1,1);w.Bits(30,2,(item.pipeline.Flags&4)?2:0);
        w.Begin(87,5);w.Bits(8,32,~item.pipeline.ColorMask);
        w.Begin(83,2);w.Bits(8,8,item.pipeline.Flags&1);
        if(item.pipeline.Flags&1){const auto &b=item.pipeline.Blend;w.Begin(84,5);w.Bits(8,4,b[5]);w.Bits(12,4,b[3]);w.Bits(16,4,b[4]);w.Bits(20,4,b[2]);w.Bits(24,4,b[0]);w.Bits(28,4,b[1]);w.Bits(32,8,1);
            w.Begin(86,9);for(unsigned i=0;i<4;++i)w.Bits(8+i*16,16,item.pipeline.Constant[i]);}
        w.Begin(91,5);w.Bits(8,4,15);w.Bits(24,16,0x3f80);
        // Each hardware interpolation-flags packet covers 24 varyings. Clear
        // the full state, then write each nonzero 24-scalar chunk at V0=chunk.
        // This matches Mesa's offset model while keeping our 32-bit UMD masks.
        auto VaryingFlags=[&](unsigned zeroCode,unsigned flagsCode,uint32_t mask){
            w.Begin(zeroCode,1);
            for(unsigned chunk=0;chunk<(PI5_MAX_VARYINGS+PI5_VARYING_FLAG_CHUNK-1)/PI5_VARYING_FLAG_CHUNK;++chunk){
                uint32_t bits=chunk?mask>>PI5_VARYING_FLAG_CHUNK:mask&0x00ffffffu;
                if(!bits)continue;
                w.Begin(flagsCode,5);w.Bits(8,4,chunk);w.Bits(16,24,bits);
            }
        };
        VaryingFlags(97,98,item.flatMask);
        VaryingFlags(99,100,item.nonPerspectiveMask);
        w.Begin(88,1);
        w.Begin(71,2);w.Bits(8,8,0x22);
        w.Begin(64,5);w.Bits(8,32,records[n] | ((item.vertexScalars+3)/4));
        w.Begin(36,10);w.Bits(8,8,4);w.Bits(16,32,item.vertexCount);
        }
        w.Begin(4,1);out.binEnd = w.Address(base);
        w.Align(64);out.renderStart = w.Address(base);
        w.Begin(121,9);w.Bits(16,16,d.width);w.Bits(32,16,d.height);w.Bits(52,1,1);w.Bits(54,1,1);w.Bits(60,3,3);w.Bits(63,3,3);
        w.Begin(121,9);w.Bits(8,3,2);w.Bits(26,7,31);w.Bits(35,5,8);w.Bits(40,32,d.clearColor);
        w.Begin(121,9);w.Bits(8,4,1);
        w.Begin(126,2);w.Bits(10,1,1);
        w.Begin(123,5);w.Bits(8,32,d.tile);
        w.Begin(122,9);w.Bits(8,8,superWidth-1);w.Bits(16,8,superHeight-1);w.Bits(24,8,superX);w.Bits(32,8,superY);w.Bits(40,12,tilesX);w.Bits(52,12,tilesY);
        for (unsigned i=0;i<2;++i) {
            w.Begin(124,4);w.Begin(26,1);TileBuffer(w,d,false,true);
            if (!i) w.Begin(25,1);
            w.Begin(27,1);
        }
        w.Begin(19,1);w.Begin(20,9);w.Bits(8,32,generic);w.Bits(40,32,genericEnd);
        // Preserve untouched supertiles between disjoint draws as well as
        // outside their combined bounds. A clear-on-load pass still visits
        // the entire target to initialize its background.
        uint32_t selected=generic;
        uint32_t clips[PI5_MAX_BATCH_DRAWS][4]={};
        if(d.loadTarget)for(uint32_t n=0;n<count;++n)ClipRect(draws[n],clips[n]);
        for (unsigned y=0;y<superY;++y) for (unsigned x=0;x<superX;++x) {
            bool touched=!d.loadTarget;
            for(uint32_t n=0;!touched&&n<count;++n){const auto&clip=clips[n];
                touched=clip[0]<clip[2]&&clip[1]<clip[3]&&
                    x*superWidth*TileWidth<clip[2]&&(x+1)*superWidth*TileWidth>clip[0]&&
                    y*superHeight*TileHeight<clip[3]&&(y+1)*superHeight*TileHeight>clip[1];
            }
            if(!touched)continue;
            uint32_t left=x*superWidth*TileWidth,top=y*superHeight*TileHeight,right=left+superWidth*TileWidth,bottom=top+superHeight*TileHeight;
            if(right>d.width)right=d.width;if(bottom>d.height)bottom=d.height;
            const auto&cover=d.pipeline.Coverage;
            bool skip=coverage&&left>=cover[0]&&top>=cover[1]&&right<=cover[2]&&bottom<=cover[3];
            uint32_t next=skip?opaque:generic;
            if(next!=selected){w.Begin(20,9);w.Bits(8,32,next);w.Bits(40,32,skip?opaqueEnd:genericEnd);selected=next;}
            if(skip)out.skippedLoads+=((right-left+TileWidth-1)/TileWidth)*((bottom-top+TileHeight-1)/TileHeight);
            w.Begin(23,3);w.Bits(8,8,x);w.Bits(16,8,y);
        }
        w.Begin(13,1);out.renderEnd=w.Address(base);Need(w.valid,"command storage or field overflow");out.bytes=w.used;return true;
    }
}
}
