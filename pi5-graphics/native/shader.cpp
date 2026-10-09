#include "shader.h"
#include "abi.h"
#include "qpu-alu.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace pi5 {
namespace {
constexpr uint32_t Missing = UINT32_MAX;
constexpr uint64_t Nop = UINT64_C(0x38003186bb03f000);
constexpr uint64_t Switch = UINT64_C(0x38203186bb03f000);

uint32_t Word(const uint8_t *p) { uint32_t x; std::memcpy(&x,p,4); return x; }
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void Need(bool condition, const char *message) { if (!condition) throw Failure(message); }
struct Reader {
    const uint8_t *data;
    size_t count, at = 0;
    uint32_t Take() { Need(at < count,"truncated shader instruction"); return Word(data + 4 * at++); }
};
struct Operand {
    uint32_t type = 0, dimensions = 0, index[2] = {}, value[4] = {};
    uint32_t swizzle[4] = {0,1,2,3}, mask = 15, modifier = 0;
    std::shared_ptr<Operand> relative[2];
};
Operand ReadOperand(Reader &r, bool destination,unsigned depth=0) {
    Need(depth<4,"relative operand nesting limit exceeded");
    Operand o;
    uint32_t t = r.Take(), components = t & 3, selection = (t >> 2) & 3;
    o.type = (t >> 12) & 255;
    o.dimensions = (t >> 20) & 3;
    Need(components == 1 || components == 2 || (!components&&(o.type==6||o.type==7||o.type==13)),"unsupported operand component count");
    Need(o.dimensions <= 2,"unsupported operand dimension");
    if (!components) {
        Need(o.type==13?destination&&!o.dimensions:!destination,"resource operand is not a destination");
    } else if (components == 1) {
        o.mask = 1;
        std::fill(std::begin(o.swizzle),std::end(o.swizzle),0);
    } else if (selection == 0) {
        o.mask = (t >> 4) & 15;
        Need(destination || o.type == 4,"source operand requires swizzle or select");
    } else if (selection == 1) {
        Need(!destination,"destination cannot have a swizzle");
        for (unsigned c = 0; c < 4; ++c) o.swizzle[c] = (t >> (4 + 2 * c)) & 3;
    } else if (selection == 2) {
        Need(!destination,"destination cannot select a component");
        std::fill(std::begin(o.swizzle),std::end(o.swizzle),(t >> 4) & 3);
    } else throw Failure("reserved operand selection mode");
    if (t & 0x80000000u) {
        uint32_t x = r.Take();
        Need((x & 0x8000003fu) == 1 && !(x & 0x7ffffc00u),"unsupported extended operand");
        o.modifier = (x >> 6) & 255;
        Need(o.modifier <= 3 && (!destination || !o.modifier),"unsupported operand modifier");
    }
    for (unsigned i = 0; i < o.dimensions; ++i) {
        unsigned kind=(t>>(22+3*i))&7;Need(kind==0||(!destination&&(kind==2||kind==3)),"unsupported shader index representation");
        if(kind!=2)o.index[i]=r.Take();if(kind)o.relative[i]=std::make_shared<Operand>(ReadOperand(r,false,depth+1));
    }
    if (o.type == 4) {
        Need(!destination && o.dimensions == 0,"invalid immediate operand");
        for (unsigned c = 0; c < (components == 1 ? 1u : 4u); ++c) o.value[c] = r.Take();
    }
    return o;
}

enum class Op { Uniform, Add, Sub, Iadd, Isub, Mul, Min, Max, And, Or, Xor, Itof, Utof, Ftoi, Ftou, Ftoin, Reciprocal, Exp, Log, Texture,
                Shl, Shr, Asr, Imin, Imax, Umin, Umax, Imul, Rsqrt, Round, Trunc, Floor, Ceil, Fdx, Fdy, Feq, Fne, Flt, Fge, Ieq, Ine, Ilt, Ult, Ftz, Fzeq, Fzne, Select };
struct Node { Op op; uint32_t a, b; Uniform uniform; uint32_t c=Missing; };
// Sample and Fetch2D write TMUT then TMUS/TMUSF; Fetch1D writes only TMUSF.
enum class Lookup { Sample, Fetch2D, Fetch1D, Constant };
struct LookupGroup { Lookup kind; uint32_t binding, words; std::array<uint32_t,4> values; };
constexpr unsigned Resources = 4, Samplers = 4;
class Compiler {
    ShaderStage stage;
    std::vector<Node> nodes;
    std::vector<std::array<uint32_t,4>> temps;
    struct Indexable {std::vector<std::array<uint32_t,4>> values;uint32_t components=0;};
    std::array<Indexable,256> indexable;
    uint32_t indexableCount=0;
    std::array<std::array<uint32_t,4>,ShaderRegisters> outputs;
    ShaderSignature outputMasks = {};
    std::array<uint32_t,14> constantSizes = {};
    ShaderSignature inputMasks = {},inputModes = {};
    std::array<std::array<uint32_t,4>,ShaderRegisters> systemInputs = {};
    std::vector<uint32_t> roots;
    uint32_t positionOutput = Missing, positionInput = Missing, instructionCount = 0,varyingScalars=0;
    const ShaderSignature *linkedVaryings;
    const ConstantBuffers *constants;
    const ShaderBlend *blend;
    unsigned cseWindow;
    std::unordered_map<uint64_t,uint32_t> expressions;
    std::vector<Uniform> specialized;
    std::array<uint32_t,Resources> resourceDimensions = {};
    std::array<bool,Resources> signedBuffers = {};
    std::array<bool,Samplers> samplerDeclared = {};
    std::vector<ShaderBinding> bindings;
    std::vector<LookupGroup> lookups;
    uint32_t constantSlot=Missing,constantWords=0;
    bool executable = false, returned = false;
    // Nodes known to hold a Direct3D boolean (all zero or all one bits).
    std::vector<bool> maskNodes;
    // if/else blocks are converted to predicated stores under activeMask.
    struct Branch { uint32_t parent, condition; bool inElse; };
    std::vector<Branch> branches;
    uint32_t activeMask = Missing,returnedMask = Missing;
    uint32_t LiveMask(uint32_t mask){if(returnedMask==Missing)return mask;uint32_t live=Not(returnedMask);return mask==Missing?live:AndMask(mask,live);}
    uint32_t Put(Node n) {
        Need(nodes.size() < 65536,"shader scalar instruction limit exceeded");
        nodes.push_back(n); maskNodes.push_back(false); return static_cast<uint32_t>(nodes.size() - 1);
    }
    uint32_t UniformNode(UniformKind kind, uint32_t value, uint32_t slot = 0) {
        for (uint32_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].op == Op::Uniform && nodes[i].uniform.kind == kind &&
                nodes[i].uniform.value == value && nodes[i].uniform.slot == slot) return i;
        return Put({Op::Uniform,Missing,Missing,{kind,value,slot}});
    }
    uint32_t Literal(uint32_t x) { return UniformNode(UniformKind::Literal,x); }
    bool IsLiteral(uint32_t node) const {return node<nodes.size()&&nodes[node].op==Op::Uniform&&
        (nodes[node].uniform.kind==UniformKind::Literal||(constants&&nodes[node].uniform.kind==UniformKind::ConstantBuffer));}
    uint32_t Value(uint32_t node) {
        Need(IsLiteral(node),"shader value is not constant");auto u=nodes[node].uniform;
        if(u.kind==UniformKind::Literal)return u.value;
        bool found=false;for(const auto&word:specialized)if(word.slot==u.slot&&word.value==u.value){found=true;break;}
        if(!found)specialized.push_back(u);
        const auto&buffer=(*constants)[u.slot];return u.value<buffer.size()?buffer[u.value]:0;
    }
    uint32_t Binary(Op op, uint32_t a, uint32_t b,bool fold=true) {
        Need(a < nodes.size() && b < nodes.size(),"read of undefined shader value");
        // A float subtraction consumes the sign modifier directly, without
        // materializing a negated operand or changing arithmetic association.
        if(op==Op::Add){
            auto Negated=[&](uint32_t id){const auto&n=nodes[id];if(n.op!=Op::Xor)return Missing;
                for(unsigned i=0;i<2;++i){uint32_t v=i?n.a:n.b;
                    if(nodes[v].op==Op::Uniform&&nodes[v].uniform.kind==UniformKind::Literal&&nodes[v].uniform.value==0x80000000u)return i?n.b:n.a;}
                return Missing;};
            uint32_t x=Negated(a),y=Negated(b);
            if(y!=Missing){op=Op::Sub;b=y;}else if(x!=Missing){op=Op::Sub;a=b;b=x;}
        }
        // Integer folding resolves loop counters and constant-table indices.
        // Float arithmetic stays on the QPU with its existing rounding rules.
        if(fold&&IsLiteral(a)&&IsLiteral(b)){
            // Do not make float uniforms part of the specialization key.
            bool integer=op==Op::Iadd||op==Op::Isub||op==Op::And||op==Op::Or||op==Op::Xor||op==Op::Shl||op==Op::Shr||op==Op::Asr||
                         op==Op::Imin||op==Op::Imax||op==Op::Umin||op==Op::Umax||op==Op::Imul;
            uint32_t x=integer?Value(a):0,y=integer?Value(b):0;
            switch(op){
            case Op::Iadd:return Literal(x+y);case Op::Isub:return Literal(x-y);
            case Op::And:return Literal(x&y);case Op::Or:return Literal(x|y);case Op::Xor:return Literal(x^y);
            case Op::Shl:return Literal(x<<(y&31));case Op::Shr:return Literal(x>>(y&31));
            case Op::Asr:{unsigned shift=y&31;return Literal(shift?(x>>shift)|((x&0x80000000u)?(~0u<<(32-shift)):0):x);}
            case Op::Imin:return Literal((x^0x80000000u)<(y^0x80000000u)?x:y);
            case Op::Imax:return Literal((x^0x80000000u)>(y^0x80000000u)?x:y);
            case Op::Umin:return Literal(std::min(x,y));case Op::Umax:return Literal(std::max(x,y));
            case Op::Imul:return Literal(x*y);default:break;
            }
        }
        if(op==Op::And||op==Op::Or||op==Op::Xor){
            if(a==b)return op==Op::Xor?Literal(0):a;
            auto Bits=[&](uint32_t node,uint32_t value){return nodes[node].op==Op::Uniform&&nodes[node].uniform.kind==UniformKind::Literal&&nodes[node].uniform.value==value;};
            if(Bits(a,0))return op==Op::And?a:b;if(Bits(b,0))return op==Op::And?b:a;
            if(op==Op::And){if(Bits(a,UINT32_MAX))return b;if(Bits(b,UINT32_MAX))return a;}
            if(op==Op::Or){if(Bits(a,UINT32_MAX))return a;if(Bits(b,UINT32_MAX))return b;}
        }
        if ((op == Op::Min || op == Op::Max || op == Op::Imin || op == Op::Imax || op == Op::Umin || op == Op::Umax) && a == b) return a;
        // Reuse identical pure scalar operations without reassociation or
        // float folding. Texture requests retain their explicit ordering.
        if(cseWindow){uint64_t key=(uint64_t(op)<<32)|(uint64_t(a)<<16)|b;
            auto found=expressions.find(key);if(found!=expressions.end()&&nodes.size()-found->second<cseWindow)return found->second;
            uint32_t result=Put({op,a,b,{UniformKind::Literal,0,0}});expressions[key]=result;return result;
        }
        return Put({op,a,b,{UniformKind::Literal,0,0}});
    }
    uint32_t Binding(uint32_t resource,uint32_t sampler){
        for(uint32_t i=0;i<bindings.size();++i)if(bindings[i].resource==resource&&bindings[i].sampler==sampler)return i;
        Need(bindings.size()<PI5_BINDINGS,"shader resource binding limit exceeded");
        bindings.push_back({resource,sampler,resourceDimensions[resource]==1,signedBuffers[resource]});return static_cast<uint32_t>(bindings.size()-1);
    }
    std::array<uint32_t,4> Fetch(Lookup kind,uint32_t binding,uint32_t s,uint32_t t,uint32_t words){
        Need(lookups.size()<PI5_MAX_LOOKUPS&&s<nodes.size()&&t<nodes.size(),"texture lookup limit exceeded");
        LookupGroup group{kind,binding,words,{Missing,Missing,Missing,Missing}};
        for(unsigned c=0;c<words;++c)group.values[c]=Put({Op::Texture,s,t,{UniformKind::Literal,c,static_cast<uint32_t>(lookups.size())}});
        lookups.push_back(group);return group.values;
    }
    // D3D returns zero for out-of-range texel fetches; limit is the element or texel count.
    uint32_t InRange(uint32_t clamped,uint32_t limit){return Binary(Op::Shr,Binary(Op::Isub,clamped,limit),Literal(31));}
    uint32_t Mask(uint32_t node){maskNodes[node]=true;return node;}
    // x | -x has its sign bit set exactly when x is nonzero.
    uint32_t NonZero(uint32_t x){if(maskNodes[x])return x;if(IsLiteral(x))return Literal(Value(x)?UINT32_MAX:0);return Mask(Binary(Op::Ine,x,Literal(0)));}
    uint32_t Not(uint32_t mask){return Mask(Binary(Op::Xor,mask,Literal(UINT32_MAX)));}
    uint32_t AndMask(uint32_t a,uint32_t b){return Mask(Binary(Op::And,a,b));}
    uint32_t Ilt(uint32_t a,uint32_t b){if(IsLiteral(a)&&IsLiteral(b))return Literal((Value(a)^0x80000000u)<(Value(b)^0x80000000u)?UINT32_MAX:0);return Mask(Binary(Op::Ilt,a,b));}
    uint32_t Ult(uint32_t a,uint32_t b){if(IsLiteral(a)&&IsLiteral(b))return Literal(Value(a)<Value(b)?UINT32_MAX:0);return Mask(Binary(Op::Ult,a,b));}
    uint32_t Ieq(uint32_t a,uint32_t b){if(IsLiteral(a)&&IsLiteral(b))return Literal(Value(a)==Value(b)?UINT32_MAX:0);return Mask(Binary(Op::Ieq,a,b));}
    uint32_t FloatCompare(Op op,uint32_t a,uint32_t b){
        // Compare with a negated copy using the same signed-zero boundary.
        auto Negated=[&](uint32_t id){const auto&n=nodes[id];if(n.op!=Op::Xor)return Missing;
            for(unsigned i=0;i<2;++i){uint32_t v=i?n.a:n.b;
                if(nodes[v].op==Op::Uniform&&nodes[v].uniform.kind==UniformKind::Literal&&nodes[v].uniform.value==0x80000000u)return i?n.b:n.a;}
            return Missing;};
        if(Negated(b)==a)return FloatCompare(op,a,Literal(0));
        if(Negated(a)==b)return FloatCompare(op,Literal(0),b);
        auto Fixed=[&](uint32_t id){return nodes[id].op==Op::Uniform&&nodes[id].uniform.kind==UniformKind::Literal;};
        bool az=Fixed(a)&&!(nodes[a].uniform.value&0x7f800000u),bz=Fixed(b)&&!(nodes[b].uniform.value&0x7f800000u);
        if(az||bz){uint32_t value=az?b:a;
            // D3D equality with zero is exactly an exponent-zero bit test.
            if(op==Op::Feq||op==Op::Fne)return Mask(Binary(op==Op::Feq?Op::Fzeq:Op::Fzne,value,Literal(0x7f800000u)));
            // Ordered thresholds at +/-FLT_MIN include all signed subnormals
            // with zero, without changing the original input or NaN behavior.
            if(az)return Mask(op==Op::Flt?Binary(Op::Fge,value,Literal(0x00800000u)):Binary(Op::Flt,value,Literal(0x00800000u)));
            return Mask(op==Op::Flt?Binary(Op::Fge,Literal(0x80800000u),value):Binary(Op::Flt,Literal(0x80800000u),value));
        }
        // Flushing the other operand cannot change its relation to a fixed
        // nonzero normal, infinity or NaN.
        if(Fixed(a)||Fixed(b))return Mask(Binary(op,a,b));
        // QPU FCMP and FADD preserve subnormal inputs; D3D comparisons flush
        // them. Clear the complete value whenever its IEEE exponent is zero.
        auto Normal=[&](uint32_t value){
            if(nodes[value].op==Op::Uniform){auto u=nodes[value].uniform;
                if(u.kind==UniformKind::Literal)return (u.value&0x7f800000u)?value:Literal(0);
                // Upload a canonicalized copy of this uniform for comparison;
                // other users retain the unmodified integer/float word.
                if(u.kind==UniformKind::ConstantBuffer)return UniformNode(UniformKind::FloatConstantBuffer,u.value,u.slot);
            }
            return Binary(Op::Ftz,value,Literal(0x7f800000u));
        };
        return Mask(Binary(op,Normal(a),Normal(b)));
    }
    uint32_t Select(uint32_t mask,uint32_t a,uint32_t b){
        if(IsLiteral(mask)){uint32_t value=Value(mask);if(!value)return b;if(value==UINT32_MAX)return a;}
        if(a==b)return a;
        auto Zero=[&](uint32_t v){return nodes[v].op==Op::Uniform&&nodes[v].uniform.kind==UniformKind::Literal&&!nodes[v].uniform.value;};
        if(Zero(a))return Binary(Op::And,b,Not(mask));
        if(Zero(b))return Binary(Op::And,a,mask);
        // Preserve raw bits with a two-instruction predicated integer copy.
        // NonZero already produced a boolean; the copy can test its input.
        const auto&condition=nodes[mask];
        if(condition.op==Op::Ine&&Zero(condition.b))mask=condition.a;
        return Put({Op::Select,mask,a,{UniformKind::Literal,0,0},b});
    }
    uint32_t Read(const Operand &o, unsigned lane, bool integer = false) {
        uint32_t v = Missing, c = o.swizzle[lane];
        Need((!o.relative[0]&&!o.relative[1])||((o.type==8||o.type==3)&&o.dimensions==2&&!o.relative[0]&&o.relative[1]),"unsupported relative shader register");
        if (o.type == 0) {
            Need(o.dimensions == 1 && o.index[0] < temps.size(),"temporary register outside declaration");
            v = temps[o.index[0]][c];
        } else if (o.type == 3) {
            Need(o.dimensions==2&&o.index[0]<indexable.size(),"invalid indexable temporary source");
            const auto&array=indexable[o.index[0]];
            Need(!array.values.empty()&&c<array.components,"indexable temporary outside declaration");
            uint32_t index=Literal(o.index[1]);
            if(o.relative[1])index=Binary(Op::Iadd,Read(*o.relative[1],0,true),index);
            if(IsLiteral(index))v=Value(index)<array.values.size()?array.values[Value(index)][c]:Literal(0);
            else{
                // Select from the current SSA values. This also handles arrays
                // initialized with literals, without CPU shader specialization.
                v=Literal(0);
                for(uint32_t i=0;i<array.values.size();++i){
                    Need(array.values[i][c]!=Missing,"indirect read of uninitialized temporary");
                    v=Select(Ieq(index,Literal(i)),array.values[i][c],v);
                }
            }
        } else if (o.type == 1) {
            Need(o.dimensions != 1 || o.index[0] != positionInput,"pixel position input reads are unsupported");
            Need(o.dimensions == 1 && o.index[0] < inputMasks.size() &&
                 (inputMasks[o.index[0]] & (1u << c)),"shader input outside declaration");
            unsigned scalar = 0;
            for (unsigned reg = 0; reg <= o.index[0]; ++reg)
                for (unsigned component = 0; component < (reg == o.index[0] ? c : 4); ++component)
                    if (inputMasks[reg] & (1u << component)) ++scalar;
            Need(scalar < (stage==ShaderStage::Pixel?PI5_MAX_VARYINGS:PI5_MAX_VERTEX_SCALARS),stage==ShaderStage::Pixel?"varying scalar limit exceeded":"vertex attribute scalar limit exceeded");
            v = UniformNode(stage==ShaderStage::Pixel?UniformKind::FragmentVarying:UniformKind::VertexAttribute,scalar);
        } else if (o.type == 4) v = Literal(o.value[c]);
        else if (o.type == 8) {
            Need(o.dimensions == 2 && o.index[0] < constantSizes.size() &&
                 o.index[1] < constantSizes[o.index[0]],"constant buffer operand outside declaration");
            if(!o.relative[1])v=UniformNode(UniformKind::ConstantBuffer,o.index[1]*4+c,o.index[0]);
            else{
                uint32_t index=Read(*o.relative[1],0,true);if(o.index[1])index=Binary(Op::Iadd,index,Literal(o.index[1]));
                if(IsLiteral(index)){
                    uint32_t element=Value(index);
                    v=element<constantSizes[o.index[0]]?UniformNode(UniformKind::ConstantBuffer,element*4+c,o.index[0]):Literal(0);
                }else{
                Need(constantSizes[o.index[0]]<=1024&&(constantSlot==Missing||constantSlot==o.index[0])&&lookups.size()<PI5_MAX_LOOKUPS&&!o.relative[1]->modifier,"unsupported indirect constant table");
                constantSlot=o.index[0];constantWords=4;while(constantWords<constantSizes[constantSlot]*4)constantWords*=2;
                uint32_t converted=Binary(Op::Utof,index,index);
                float scale=4.0f/float(constantWords),bias=(float(c)+.5f)/float(constantWords);uint32_t scaleBits,biasBits;memcpy(&scaleBits,&scale,4);memcpy(&biasBits,&bias,4);
                uint32_t s=Binary(Op::Add,Binary(Op::Mul,converted,Literal(scaleBits)),Literal(biasBits)),t=Literal(0x3f000000);
                v=Fetch(Lookup::Constant,0,s,t,4)[0];
                }
            }
        } else throw Failure("unsupported shader source register");
        Need(v != Missing,"read of uninitialized temporary register");
        if (integer) {
            Need(o.modifier <= 1,"unsupported integer source modifier");
            return o.modifier ? Binary(Op::Isub,Literal(0),v) : v;
        }
        if (o.modifier & 2) v = Binary(Op::And,v,Literal(0x7fffffffu),false);
        if (o.modifier & 1) v = Binary(Op::Xor,v,Literal(0x80000000u),false);
        return v;
    }
    void Store(const Operand &o, const std::array<uint32_t,4> &value) {
        Need(o.mask && o.dimensions==(o.type==3?2u:1u),"invalid shader destination");
        std::array<uint32_t,4> *destination;
        if (o.type == 0) {
            Need(o.index[0] < temps.size(),"temporary destination outside declaration");
            destination = &temps[o.index[0]];
        } else if(o.type==3){
            Need(o.index[0]<indexable.size(),"invalid indexable temporary destination");
            auto&array=indexable[o.index[0]];
            Need(o.index[1]<array.values.size()&&!(o.mask&~((1u<<array.components)-1)),"indexable temporary destination outside declaration");
            destination=&array.values[o.index[1]];
        } else {
            Need(o.type == 2 && o.index[0] < outputs.size() && !(o.mask & ~outputMasks[o.index[0]]),"unsupported shader output");
            destination = &outputs[o.index[0]];
        }
        for (unsigned c = 0; c < 4; ++c) if (o.mask & (1u << c)) {
            uint32_t old = (*destination)[c];
            (*destination)[c] = activeMask == Missing ? value[c] : Select(activeMask,value[c],old == Missing ? Literal(0) : old);
        }
    }
    void Instruction(uint32_t token, Reader &r) {
        uint32_t op = token & 0x7ff;
        Need(!(token & 0x80000000u),"extended instruction is unsupported");
        Need(!returned,"instruction after shader return");
        if (op == 104) {
            Need(!executable && temps.empty(),"invalid temporary declaration order");
            uint32_t count = r.Take(); Need(count <= 256,"too many temporary registers");
            temps.resize(count,{Missing,Missing,Missing,Missing}); return;
        }
        if(op==105){
            uint32_t id=r.Take(),count=r.Take(),components=r.Take();
            Need(!executable&&!(token&0x00fff800u)&&id<indexable.size()&&indexable[id].values.empty()&&
                 count&&count<=4096-indexableCount&&components>=1&&components<=4,"invalid indexable temporary declaration");
            indexable[id].values.resize(count,{Missing,Missing,Missing,Missing});indexable[id].components=components;indexableCount+=count;return;
        }
        if(op==96){
            Operand d=ReadOperand(r,true);uint32_t semantic=r.Take();
            Need(!executable&&stage!=ShaderStage::Pixel&&(semantic==6||semantic==8)&&d.type==1&&d.dimensions==1&&
                 d.index[0]<inputMasks.size()&&d.mask&&!(d.mask&(d.mask-1))&&!(inputMasks[d.index[0]]&d.mask),"unsupported vertex system value");
            inputMasks[d.index[0]]|=d.mask;
            for(unsigned c=0;c<4;++c)if(d.mask&(1u<<c))systemInputs[d.index[0]][c]=semantic;
            return;
        }
        if (op == 95 || op == 98) {
            Operand d = ReadOperand(r,true);
            uint32_t mode=(token>>11)&15;
            Need(!executable && (op==95?stage!=ShaderStage::Pixel:stage==ShaderStage::Pixel&&(mode==1||mode==2||mode==4)) && d.type == 1 && d.dimensions == 1 &&
                 d.index[0] < inputMasks.size() && d.mask && !(inputMasks[d.index[0]] & d.mask) && (!inputMasks[d.index[0]] || inputModes[d.index[0]] == mode),"invalid shader input declaration");
            inputMasks[d.index[0]] |= d.mask; inputModes[d.index[0]]=mode; return;
        }
        if (op == 100) {
            // SV_Position is rasterizer state rather than a varying; it may be declared but not read.
            Operand d = ReadOperand(r,true);
            Need(!executable && stage==ShaderStage::Pixel && r.Take()==1 && d.type==1 && d.dimensions==1 && d.index[0]<inputMasks.size() &&
                 !inputMasks[d.index[0]] && positionInput==Missing,"unsupported pixel system value");
            positionInput=d.index[0]; return;
        }
        if (op == 101 || op == 103) {
            Operand d = ReadOperand(r,true);
            if(op==103){Need(stage!=ShaderStage::Pixel&&r.Take()==1&&positionOutput==Missing,"unsupported output semantic");positionOutput=d.index[0];}
            Need(!executable && d.type == 2 && d.dimensions == 1 && d.index[0]<outputs.size() && (stage!=ShaderStage::Pixel||d.index[0]<=1) && d.mask &&
                 !(d.mask & outputMasks[d.index[0]]),"invalid output declaration");
            outputMasks[d.index[0]] |= d.mask; return;
        }
        if (op == 89) {
            Operand d = ReadOperand(r,false);
            Need(!executable && d.type == 8 && d.dimensions == 2 && d.index[0] < 14 &&
                 d.index[1] && d.index[1] <= 4096 && !constantSizes[d.index[0]] &&
                 !(token & 0x00fff000u),"invalid constant buffer declaration");
            constantSizes[d.index[0]] = d.index[1]; return;
        }
        if (op == 106) { Need(!executable && !(token & 0x00fff000u),"unsupported shader global flags"); return; }
        if(op==88){
            // Integer buffer views use their bound component format; textures return float color.
            Operand d=ReadOperand(r,false);uint32_t dimension=(token>>11)&31,returns=r.Take();
            Need(!executable&&(stage==ShaderStage::Pixel||dimension==1)&&d.type==7&&d.dimensions==1&&d.index[0]<Resources&&!resourceDimensions[d.index[0]]&&!(token&0x00ff0000u)&&
                 ((dimension==1&&(returns==0x4444||returns==0x3333))||(dimension==3&&returns==0x5555)),"unsupported texture declaration");
            resourceDimensions[d.index[0]]=dimension;signedBuffers[d.index[0]]=dimension==1&&returns==0x3333;return;
        }
        if(op==90){Operand d=ReadOperand(r,false);Need(!executable&&stage==ShaderStage::Pixel&&d.type==6&&d.dimensions==1&&d.index[0]<Samplers&&!(token&0x00fff800u)&&!samplerDeclared[d.index[0]],"unsupported sampler declaration");samplerDeclared[d.index[0]]=true;return;}
        executable = true;
        if (op == 62) {
            Need(!(token&0x00fff800u),"unsupported return controls");
            if(branches.empty())returned=true;
            else{Need(activeMask!=Missing,"missing conditional return mask");returnedMask=returnedMask==Missing?activeMask:Binary(Op::Or,returnedMask,activeMask);activeMask=Literal(0);}
            return;
        }
        if (op == 58) return;
        if (op == 31) {
            Need(!(token & 0x00fbf800u),"unsupported conditional controls");
            Operand s = ReadOperand(r,false);uint32_t condition = NonZero(Read(s,0,true));
            if (!(token & (1u << 18))) condition = Not(condition);
            branches.push_back({activeMask,condition,false});
            activeMask = activeMask == Missing ? condition : AndMask(activeMask,condition);return;
        }
        if (op == 18) {
            Need(!(token & 0x00fff800u) && !branches.empty() && !branches.back().inElse,"unbalanced else");
            auto &b = branches.back();b.inElse = true;uint32_t inverse = Not(b.condition);
            activeMask = LiveMask(b.parent == Missing ? inverse : AndMask(b.parent,inverse));return;
        }
        if (op == 21) {
            Need(!(token & 0x00fff800u) && !branches.empty(),"unbalanced endif");
            activeMask = LiveMask(branches.back().parent);branches.pop_back();return;
        }
        if (op == 55) {
            Need(!(token & 0x00ffd800u),"unsupported instruction controls");
            Operand d = ReadOperand(r,true), condition = ReadOperand(r,false), a = ReadOperand(r,false), b = ReadOperand(r,false);
            std::array<uint32_t,4> result = {Missing,Missing,Missing,Missing};
            for (unsigned c = 0; c < 4; ++c) if (d.mask & (1u << c)) result[c] = Select(NonZero(Read(condition,c,true)),Read(a,c),Read(b,c));
            if (token & 0x2000) for (unsigned c = 0; c < 4; ++c) if (d.mask & (1u << c))
                result[c] = Binary(Op::Min,Binary(Op::Max,result[c],Literal(0)),Literal(0x3f800000));
            Store(d,result);return;
        }
        if(op==69){
            Need(stage==ShaderStage::Pixel&&!(token&0x00fff800u),"unsupported texture sample");
            Operand d=ReadOperand(r,true),uv=ReadOperand(r,false),texture=ReadOperand(r,false),sampler=ReadOperand(r,false);
            Need(texture.type==7&&texture.dimensions==1&&texture.index[0]<Resources&&resourceDimensions[texture.index[0]]==3&&!texture.modifier&&
                 sampler.type==6&&sampler.dimensions==1&&sampler.index[0]<Samplers&&samplerDeclared[sampler.index[0]]&&!sampler.modifier,"unsupported texture binding");
            auto values=Fetch(Lookup::Sample,Binding(texture.index[0],sampler.index[0]),Read(uv,0),Read(uv,1),4);
            std::array<uint32_t,4> result;for(unsigned c=0;c<4;++c)result[c]=values[texture.swizzle[c]];Store(d,result);return;
        }
        if(op==45){
            Need(!(token&0x00fff800u),"unsupported texel fetch");
            Operand d=ReadOperand(r,true),address=ReadOperand(r,false),texture=ReadOperand(r,false);
            Need(texture.type==7&&texture.dimensions==1&&texture.index[0]<Resources&&resourceDimensions[texture.index[0]]&&(stage==ShaderStage::Pixel||resourceDimensions[texture.index[0]]==1)&&!texture.modifier&&!address.modifier,"unsupported texel fetch binding");
            uint32_t binding=Binding(texture.index[0],NoSampler);std::array<uint32_t,4> values;
            if(resourceDimensions[texture.index[0]]==1){
                // The kernel appends one zero element, so a clamped index past the view reads zero.
                uint32_t count=UniformNode(UniformKind::BindingElements,0,binding),index=Binary(Op::Umin,Read(address,0,true),count);
                // The descriptor supplies sign/zero extension and missing-channel defaults.
                values=Fetch(Lookup::Fetch1D,binding,index,index,4);
            }else{
                uint32_t level=Read(address,3,true);
                Need(nodes[level].op==Op::Uniform&&nodes[level].uniform.kind==UniformKind::Literal&&!nodes[level].uniform.value,"texel fetch mip level must be zero");
                uint32_t width=UniformNode(UniformKind::BindingWidth,0,binding),height=UniformNode(UniformKind::BindingHeight,0,binding);
                uint32_t x=Binary(Op::Umin,Read(address,0,true),width),y=Binary(Op::Umin,Read(address,1,true),height);
                uint32_t mask=Binary(Op::Isub,Literal(0),Binary(Op::And,InRange(x,width),InRange(y,height)));
                auto texels=Fetch(Lookup::Fetch2D,binding,x,y,4);for(unsigned c=0;c<4;++c)values[c]=Binary(Op::And,texels[c],mask);
            }
            std::array<uint32_t,4> result;for(unsigned c=0;c<4;++c)result[c]=values[texture.swizzle[c]];Store(d,result);return;
        }
        if(op==38||op==81){
            Need(!(token&0x00fff800u),"unsupported instruction controls");
            Operand high=ReadOperand(r,true),low=ReadOperand(r,true),a=ReadOperand(r,false),b=ReadOperand(r,false);
            Need(high.type==13,"high multiply result is unsupported");
            if(low.type==13)return;
            std::array<uint32_t,4> result={Missing,Missing,Missing,Missing};
            for(unsigned c=0;c<4;++c)if(low.mask&(1u<<c))result[c]=Binary(Op::Imul,Read(a,c,true),Read(b,c,true));
            Store(low,result);return;
        }
        bool comparison = op == 24 || op == 29 || op == 49 || op == 57 || (op >= 32 && op <= 34) || op == 39 || op == 79 || op == 80;
        bool integer = op == 30 || op == 35 || op == 36 || op == 37 || op == 40 || op == 41 || op == 42 || op == 59 || (op >= 82 && op <= 85) ||
                       (op >= 32 && op <= 34) || op == 39 || op == 79 || op == 80;
        bool unary = op == 54 || op == 27 || op == 28 || op == 40 || op == 43 || op == 86 || op == 59 || op == 26 || op == 25 || op == 47 || (op >= 64 && op <= 68) || op == 75 || op == 11 || op == 12;
        unsigned count = unary ? 1 : op == 50 || op == 35 || op == 82 ? 3 : 2;
        Need(op == 0 || op == 1 || (op >= 15 && op <= 17) || op == 50 || op == 51 ||
             op == 14 || op == 30 || op == 40 || op == 52 || op == 54 || op == 56 || op == 60 || op == 87 || op == 27 || op == 28 || op == 43 || op == 86 ||
             integer || comparison || op == 26 || op == 25 || op == 47 || (op >= 64 && op <= 68) || op == 75 || ((op == 11 || op == 12) && stage == ShaderStage::Pixel),"unsupported DXBC opcode");
        Need(!(token & 0x00ffd800u),"unsupported instruction controls");
        Need((!integer && !comparison) || !(token & 0x2000),"integer arithmetic cannot saturate");
        Operand d = ReadOperand(r,true), sources[3];
        for (unsigned i = 0; i < count; ++i) sources[i] = ReadOperand(r,false);
        std::array<uint32_t,4> result = {Missing,Missing,Missing,Missing};
        if (op >= 15 && op <= 17) {
            uint32_t dot = Missing;
            for (unsigned c = 0; c < op - 13; ++c) {
                uint32_t term = Binary(Op::Mul,Read(sources[0],c),Read(sources[1],c));
                dot = dot == Missing ? term : Binary(Op::Add,dot,term);
            }
            result.fill(dot);
        } else {
            for (unsigned c = 0; c < 4; ++c) if (d.mask & (1u << c)) {
                uint32_t a = Read(sources[0],c,integer), b = count > 1 ? Read(sources[1],c,integer) : Missing;
                switch (op) {
                case 24: result[c] = FloatCompare(Op::Feq,a,b); break;
                case 29: result[c] = FloatCompare(Op::Fge,a,b); break;
                case 49: result[c] = FloatCompare(Op::Flt,a,b); break;
                case 57: result[c] = FloatCompare(Op::Fne,a,b); break;
                case 32: result[c] = Ieq(a,b); break;
                case 33: result[c] = Not(Ilt(a,b)); break;
                case 34: result[c] = Ilt(a,b); break;
                case 39: result[c] = NonZero(Binary(Op::Xor,a,b)); break;
                case 79: result[c] = Ult(a,b); break;
                case 80: result[c] = Not(Ult(a,b)); break;
                case 26: result[c] = Binary(Op::Add,a,Binary(Op::Xor,Binary(Op::Floor,a,a),Literal(0x80000000u))); break;
                case 64: result[c] = Binary(Op::Round,a,a); break;
                case 65: result[c] = Binary(Op::Floor,a,a); break;
                case 66: result[c] = Binary(Op::Ceil,a,a); break;
                case 67: result[c] = Binary(Op::Trunc,a,a); break;
                case 68: result[c] = Binary(Op::Rsqrt,a,a); break;
                case 25: result[c] = Binary(Op::Exp,a,a); break;
                case 47: result[c] = Binary(Op::Log,a,a); break;
                case 75: {uint32_t rsq=Binary(Op::Rsqrt,a,a);result[c]=Binary(Op::Reciprocal,rsq,rsq);break;}
                case 11: result[c] = Binary(Op::Fdx,a,a); break;
                case 12: result[c] = Binary(Op::Fdy,a,a); break;
                case 35: case 82: result[c] = Binary(Op::Iadd,Binary(Op::Imul,a,b),Read(sources[2],c,true)); break;
                case 36: result[c] = Binary(Op::Imax,a,b); break;
                case 37: result[c] = Binary(Op::Imin,a,b); break;
                case 41: result[c] = Binary(Op::Shl,a,b); break;
                case 42: result[c] = Binary(Op::Asr,a,b); break;
                case 59: result[c] = Binary(Op::Xor,a,Literal(UINT32_MAX)); break;
                case 83: result[c] = Binary(Op::Umax,a,b); break;
                case 84: result[c] = Binary(Op::Umin,a,b); break;
                case 85: result[c] = Binary(Op::Shr,a,b); break;
                case 54: result[c] = a; break;
                case 0: result[c] = Binary(Op::Add,a,b); break;
                case 1: result[c] = Binary(Op::And,a,b); break;
                case 14: result[c] = Binary(Op::Mul,a,Binary(Op::Reciprocal,b,b)); break;
                case 27: result[c] = Binary(Op::Ftoi,a,a); break;
                case 28: result[c] = Binary(Op::Ftou,a,a); break;
                case 30: result[c] = Binary(Op::Iadd,a,b); break;
                case 40: result[c] = Binary(Op::Isub,Literal(0),a); break;
                case 43: result[c] = Binary(Op::Itof,a,a); break;
                case 50: result[c] = Binary(Op::Add,Binary(Op::Mul,a,b),Read(sources[2],c)); break;
                case 51: result[c] = Binary(Op::Min,a,b); break;
                case 52: result[c] = Binary(Op::Max,a,b); break;
                case 56: result[c] = Binary(Op::Mul,a,b); break;
                case 60: result[c] = Binary(Op::Or,a,b); break;
                case 86: result[c] = Binary(Op::Utof,a,a); break;
                case 87: result[c] = Binary(Op::Xor,a,b); break;
                }
            }
        }
        if (token & 0x2000) for (unsigned c = 0; c < 4; ++c) if (d.mask & (1u << c))
            result[c] = Binary(Op::Min,Binary(Op::Max,result[c],Literal(0)),Literal(0x3f800000));
        Store(d,result);
    }
    static uint64_t Add(unsigned op, unsigned destination, unsigned a, unsigned b, bool magic = false) {
        uint64_t word = Nop & ~((UINT64_C(1) << 44) | (UINT64_C(63) << 32) | (UINT64_C(255) << 24) | UINT64_C(4095));
        return word | (uint64_t(magic) << 44) | (uint64_t(destination) << 32) |
               (uint64_t(op) << 24) | (uint64_t(a) << 6) | b;
    }
public:
    explicit Compiler(ShaderStage s,const ShaderSignature *link=nullptr,const ConstantBuffers *values=nullptr,const ShaderBlend *blending=nullptr,unsigned cse=64) : stage(s),linkedVaryings(link),constants(values),blend(blending),cseWindow(cse) {for(auto &o:outputs)o.fill(Missing);}
    Shader Run(const uint8_t *data, size_t words) {
        Reader r{data,words};
        Need(r.Take() == (stage == ShaderStage::Pixel ? 0x40u : 0x10040u),"shader stage or model is unsupported");
        Need(r.Take() == words,"shader token count mismatch");
        // Match structured blocks before following a specialized branch. A loop
        // may not jump across an if/endif pair, even when it has zero iterations.
        std::vector<size_t> ends(words,0);
        std::vector<std::pair<size_t,uint32_t>> blocks;
        for(size_t at=2;at<words;){
            uint32_t token=Word(data+4*at),op=token&0x7ff,length=(token>>24)&127;
            Need(length&&length<=words-at,"invalid shader instruction length");
            if(op==48||op==31){Need(blocks.size()<16,"shader control nesting limit exceeded");blocks.emplace_back(at,op);}
            if(op==18){Need(!blocks.empty()&&blocks.back().second==31,"unbalanced else");}
            if(op==22||op==21){Need(!blocks.empty()&&blocks.back().second==(op==22?48u:31u),"unbalanced shader control block");ends[blocks.back().first]=at+length;blocks.pop_back();}
            at+=length;
        }
        Need(blocks.empty(),"unterminated shader control block");
        struct Loop {size_t body,end,branchDepth;uint32_t mask,iterations;};
        std::vector<Loop> loops;
        while (r.at < r.count) {
            size_t start = r.at;
            uint32_t t = r.Take(), length = (t >> 24) & 127;
            Need(length && length <= r.count - start,"invalid shader instruction length");
            Reader instruction{data + 4 * (start + 1),length - 1};
            size_t next=start+length;
            try {
                Need(++instructionCount<=65536,"shader specialization instruction limit exceeded");
                uint32_t op=t&0x7ff;
                if(op==48){
                    Need(!(t&0x80fff800u)&&length==1&&!returned,"invalid loop instruction");
                    Need(activeMask==Missing||IsLiteral(activeMask),"divergent loop is unsupported");
                    executable=true;
                    if(activeMask!=Missing&&!Value(activeMask))next=ends[start];
                    else loops.push_back({next,ends[start],branches.size(),activeMask,0});
                }else if(op==22){
                    Need(!(t&0x80fff800u)&&length==1&&!loops.empty()&&branches.size()==loops.back().branchDepth,"invalid endloop instruction");
                    Need(++loops.back().iterations<=128,"uniform loop iteration limit exceeded");
                    next=loops.back().body;
                }else if(op==2||op==3){
                    Need(!(t&(op==3?0x80fbf800u:0x80fff800u))&&!loops.empty(),"invalid loop break");
                    uint32_t condition=UINT32_MAX;
                    if(op==3){Operand operand=ReadOperand(instruction,false);uint32_t value=NonZero(Read(operand,0,true));Need(IsLiteral(value),"loop exit must be uniform and constant");condition=Value(value);if(!(t&(1u<<18)))condition=~condition;}
                    if(condition){
                        Need(activeMask==Missing||IsLiteral(activeMask),"divergent loop break is unsupported");
                        if(activeMask==Missing||Value(activeMask)){auto loop=loops.back();next=loop.end;branches.resize(loop.branchDepth);activeMask=loop.mask;loops.pop_back();}
                    }
                }else{Need(op!=62||loops.empty(),"return inside a loop is unsupported");Instruction(t,instruction);}
            }
            catch (const Failure &e) { throw Failure(std::string(e.what()) + " at token " + std::to_string(start) + " opcode " + std::to_string(t & 0x7ff)); }
            Need(instruction.at == instruction.count,"unconsumed shader instruction data");
            r.at = next;
        }
        uint32_t selected=stage==ShaderStage::Pixel?0:positionOutput;
        Need(returned && selected<outputs.size() && outputMasks[selected] == 15,"shader must return a complete position/color output");
        const auto &output=outputs[selected];
        for (auto v : output) Need(v != Missing,"shader color component is uninitialized");
        if (stage == ShaderStage::Pixel) {
            roots.assign(output.begin(),output.end());
            if(blend&&blend->functions[2]){
                auto Saturate=[&](uint32_t value){return Binary(Op::Min,Binary(Op::Max,value,Literal(0)),Literal(0x3f800000));};
                auto Invert=[&](uint32_t value){return Binary(Op::Add,Literal(0x3f800000),Binary(Op::Xor,value,Literal(0x80000000)));};
                std::array<uint32_t,4> source,destination;
                for(unsigned c=0;c<4;++c){source[c]=Saturate(output[c]);destination[c]=UniformNode(UniformKind::TargetColor,c);}
                if(blend->opaqueTarget)destination[3]=Literal(0x3f800000);
                auto Factor=[&](uint32_t factor,unsigned c){
                    switch(factor){
                    case 1:return Literal(0);case 2:return Literal(0x3f800000);
                    case 3:return source[c];case 4:return Invert(source[c]);case 5:return source[3];case 6:return Invert(source[3]);
                    case 7:return destination[3];case 8:return Invert(destination[3]);case 9:return destination[c];case 10:return Invert(destination[c]);
                    case 11:return c==3?Literal(0x3f800000):Binary(Op::Min,source[3],Invert(destination[3]));
                    case 14:case 15:case 20:case 21:{uint32_t value=Saturate(UniformNode(UniformKind::BlendConstant,factor>=20?3:c));return factor==15||factor==21?Invert(value):value;}
                    case 16:case 17:case 18:case 19:{unsigned component=factor>=18?3:c;Need(outputs[1][component]!=Missing,"dual-source blend output is uninitialized");uint32_t value=Saturate(outputs[1][component]);return factor==17||factor==19?Invert(value):value;}
                    default:throw Failure("unsupported shader blend factor");
                    }
                };
                for(unsigned c=0;c<4;++c){unsigned base=c==3?3:0;uint32_t operation=blend->functions[base+2];Need(operation>=1&&operation<=5,"unsupported shader blend operation");
                    if(operation>=4)roots[c]=Binary(operation==4?Op::Min:Op::Max,source[c],destination[c]);
                    else{uint32_t a=Binary(Op::Mul,source[c],Factor(blend->functions[base],c)),b=Binary(Op::Mul,destination[c],Factor(blend->functions[base+1],c));
                        if(operation==2)b=Binary(Op::Xor,b,Literal(0x80000000));if(operation==3)a=Binary(Op::Xor,a,Literal(0x80000000));roots[c]=Binary(Op::Add,a,b);}
                }
            }
            for(auto mask:inputMasks)for(unsigned c=0;c<4;++c)if(mask&(1u<<c))++varyingScalars;
        }
        else {
            uint32_t reciprocal = Binary(Op::Reciprocal,output[3],output[3]);
            if (stage == ShaderStage::Coordinate) roots.assign(output.begin(),output.end());
            for (unsigned c = 0; c < 2; ++c) {
                uint32_t scaled = Binary(Op::Mul,output[c],UniformNode(UniformKind::Viewport,c));
                uint32_t projected = Binary(Op::Mul,scaled,reciprocal);
                roots.push_back(Binary(Op::Ftoin,projected,projected));
            }
            if (stage == ShaderStage::Vertex) {
                uint32_t scaled = Binary(Op::Mul,output[2],UniformNode(UniformKind::Viewport,2));
                roots.push_back(Binary(Op::Add,Binary(Op::Mul,scaled,reciprocal),UniformNode(UniformKind::Viewport,3)));
                roots.push_back(reciprocal);
                for(unsigned reg=0;reg<outputs.size();++reg){uint32_t mask=linkedVaryings?(*linkedVaryings)[reg]:(reg==positionOutput?0:outputMasks[reg]);
                    Need(reg!=positionOutput||!mask,"position fragment inputs are unsupported");Need(!(mask&~outputMasks[reg]),"vertex/pixel signature mismatch");
                    for(unsigned c=0;c<4;++c)if(mask&(1u<<c)){Need(outputs[reg][c]!=Missing,"uninitialized vertex varying");roots.push_back(outputs[reg][c]);++varyingScalars;}
                }
            }
        }
        Need(varyingScalars<=PI5_MAX_VARYINGS,"varying scalar limit exceeded");return Emit();
    }
    Shader Emit();
};

Shader Compiler::Emit() {
    Shader result;
    std::vector<size_t> packable;
    auto Plain=[&](uint64_t word){packable.push_back(result.code.size());result.code.push_back(word);};
    result.instructions = instructionCount;
    result.specialized = specialized;
    result.inputs = inputMasks;
    result.systemInputs = systemInputs;
    result.outputScalars = static_cast<uint32_t>(roots.size());
    result.varyingScalars = varyingScalars;
    if(stage==ShaderStage::Pixel){unsigned scalar=0;for(unsigned reg=0;reg<inputMasks.size();++reg)for(unsigned c=0;c<4;++c)if(inputMasks[reg]&(1u<<c)){if(inputModes[reg]==4)result.nonPerspectiveMask|=1u<<scalar;if(inputModes[reg]==1)result.flatMask|=1u<<scalar;++scalar;}}
    std::vector<bool> live(nodes.size());
    std::vector<unsigned> uses(nodes.size()), registers(nodes.size(),Missing);
    for (auto v : roots) {live[v] = true; ++uses[v];}
    for (size_t i = nodes.size(); i-- > 0;) if (live[i] && nodes[i].op != Op::Uniform) {
        live[nodes[i].a] = live[nodes[i].b] = true;
        ++uses[nodes[i].a]; ++uses[nodes[i].b];
        if(nodes[i].c!=Missing){live[nodes[i].c]=true;++uses[nodes[i].c];}
    }
    // Only bindings with emitted lookups are reported, numbered in first-use order.
    std::vector<uint32_t> remap(bindings.size(),Missing);
    for(uint32_t i=0;i<nodes.size();++i)if(live[i]&&nodes[i].op==Op::Texture){const auto&l=lookups[nodes[i].uniform.slot];
        if(l.kind!=Lookup::Constant&&remap[l.binding]==Missing){remap[l.binding]=static_cast<uint32_t>(result.bindings.size());result.bindings.push_back(bindings[l.binding]);}}
    auto Remap=[&](uint32_t binding){Need(binding<remap.size()&&remap[binding]!=Missing,"texel fetch result is unused");return remap[binding];};
    bool occupied[32] = {};
    // Released uniform registers still contain their value until overwritten.
    // Reuse those bits without reserving registers or extending live ranges.
    uint32_t uniformValue[32];std::fill(std::begin(uniformValue),std::end(uniformValue),Missing);
    std::vector<bool> sampleEmitted(lookups.size());
    // RF3 is the pixel-centre W payload; each LDVARY also writes coefficient C to RF0.
    // Varyings must be read in order, but each is loaded only when first needed
    // and interpolated in its own register, keeping register pressure low.
    if(stage==ShaderStage::Pixel&&varyingScalars)occupied[0]=occupied[3]=true;
    unsigned loadedVaryings=0,lastLiveVarying=0;
    for(uint32_t i=0;i<nodes.size();++i)if(live[i]&&nodes[i].op==Op::Uniform&&nodes[i].uniform.kind==UniformKind::FragmentVarying)
        lastLiveVarying=std::max(lastLiveVarying,nodes[i].uniform.value);
    auto LoadVaryings=[&](unsigned through){
        // Drain trailing unused inputs with the last useful varying. RF0/RF3
        // can then hold ordinary values for the rest of the shader.
        if(through>=lastLiveVarying)through=varyingScalars-1;
        unsigned before=loadedVaryings;
        for(;loadedVaryings<=through&&loadedVaryings<varyingScalars;++loadedVaryings){
            unsigned scalar=loadedVaryings,reg=0;while(reg<32&&occupied[reg])++reg;Need(reg<32,"varying register pressure exceeded");
            for(uint32_t i=0;i<nodes.size();++i)if(live[i]&&nodes[i].op==Op::Uniform&&nodes[i].uniform.kind==UniformKind::FragmentVarying&&nodes[i].uniform.value==scalar){occupied[reg]=true;registers[i]=reg;break;}
            uniformValue[reg]=uniformValue[0]=Missing;
            result.code.push_back(UINT64_C(0x39003186bb03f000)|(uint64_t(reg)<<46));
            // The multiply fills LDVARY's delayed RF0 coefficient slot. Flat
            // and nonperspective inputs keep a NOP because they read RF0 next.
            // Flat varyings preserve the unmodified coefficient bits.
            bool flat=(result.flatMask&(1u<<scalar))!=0;
            uint64_t mul=Nop&~((UINT64_C(63)<<58)|(UINT64_C(1)<<45)|(UINT64_C(63)<<38)|(UINT64_C(4095)<<12));
            if(!flat&&!(result.nonPerspectiveMask&(1u<<scalar)))result.code.push_back(mul|(UINT64_C(21)<<58)|(uint64_t(reg)<<38)|(uint64_t(reg)<<18)|(UINT64_C(3)<<12));else result.code.push_back(Nop);
            result.code.push_back(flat?Add(182,reg,0,0):Add(5,reg,0,reg));
        }
        if(before<varyingScalars&&loadedVaryings==varyingScalars)occupied[0]=occupied[3]=false;
    };
    unsigned liveVertexAttributes=0,loadedVertexAttributes=0;
    for(uint32_t i=0;i<nodes.size();++i)
        if(live[i]&&nodes[i].op==Op::Uniform&&nodes[i].uniform.kind==UniformKind::VertexAttribute)++liveVertexAttributes;
    auto LoadVertexAttribute=[&](uint32_t node){
        if(stage==ShaderStage::Pixel||node>=nodes.size()||nodes[node].op!=Op::Uniform||
           nodes[node].uniform.kind!=UniformKind::VertexAttribute||registers[node]!=Missing)return;
        unsigned reg=0;while(reg<32&&occupied[reg])++reg;Need(reg<32,"vertex attribute register pressure exceeded");
        occupied[reg]=true;registers[node]=reg;uniformValue[reg]=Missing;
        result.code.push_back(UINT64_C(0x39c02180bc03f000)|(uint64_t(reg)<<32)|(uint64_t(nodes[node].uniform.value)<<6));
        result.code.push_back(Nop);++loadedVertexAttributes;
    };
    auto Ensure=[&](uint32_t node){
        if(node>=nodes.size()||nodes[node].op!=Op::Uniform)return;
        if(nodes[node].uniform.kind==UniformKind::FragmentVarying)LoadVaryings(nodes[node].uniform.value);
        else if(nodes[node].uniform.kind==UniformKind::VertexAttribute)LoadVertexAttribute(node);
    };
    // Literals, constants and draw parameters are reloaded from the uniform
    // stream at each use rather than occupying a register across the program.
    auto Reloaded=[&](uint32_t node){const auto&n=nodes[node];return n.op==Op::Uniform&&n.uniform.kind!=UniformKind::FragmentVarying&&n.uniform.kind!=UniformKind::VertexAttribute&&n.uniform.kind!=UniformKind::TargetColor;};
    auto LoadUniform=[&](uint32_t node){
        for(unsigned cached=0;cached<32;++cached)if(!occupied[cached]&&uniformValue[cached]==node){occupied[cached]=true;return cached;}
        // Put reloadable values in unused high registers. Short-lived ALU
        // results grow from the low end, preserving cached constants longer.
        unsigned reg=32;
        for(unsigned candidate=32;candidate-->0;)if(!occupied[candidate]&&uniformValue[candidate]==Missing){reg=candidate;break;}
        if(reg==32)for(unsigned candidate=32;candidate-->0;)if(!occupied[candidate]){reg=candidate;break;}
        Need(reg<32,"uniform register pressure exceeded");occupied[reg]=true;
        uniformValue[reg]=node;auto uniform=nodes[node].uniform;
        if(uniform.kind==UniformKind::Literal){unsigned immediate=48;
            if(uniform.value<16)immediate=uniform.value;
            else if(uniform.value>=0xfffffff0u)immediate=16+(uniform.value-0xfffffff0u);
            else for(unsigned n=0;n<16;++n)if(uniform.value==0x3b800000u+(n<<23)){immediate=32+n;break;}
            if(immediate<48){Plain(Add(249,reg,immediate,3)|(UINT64_C(14)<<53));return reg;}
        }
        if(uniform.kind==UniformKind::BindingElements||uniform.kind==UniformKind::BindingWidth||uniform.kind==UniformKind::BindingHeight)uniform.slot=Remap(uniform.slot);
        // V3D 7.1 register-file uniform loads are available to the next
        // instruction. Only LDVARY's delayed RF0 coefficient needs a gap.
        Plain(UINT64_C(0x39803186bb03f000)|(uint64_t(reg)<<46));result.uniforms.push_back(uniform);
        return reg;
    };
    auto Immediate=[&](uint32_t node)->unsigned{
        const auto&n=nodes[node];if(n.op!=Op::Uniform||n.uniform.kind!=UniformKind::Literal)return Missing;
        uint32_t value=n.uniform.value;if(value<16)return value;if(value>=0xfffffff0u)return 16+value-0xfffffff0u;
        for(unsigned i=0;i<16;++i)if(value==0x3b800000u+(i<<23))return 32+i;return Missing;
    };
    struct Operands{unsigned a,b,temps[2],count;bool immediateA,immediateB;};
    auto Fetch=[&](uint32_t na,uint32_t nb,bool immediate=false){Operands o={};
        unsigned ia=immediate?Immediate(na):Missing,ib=immediate?Immediate(nb):Missing;
        // A signal selects exactly one of the four ALU source fields.
        o.immediateB=ib!=Missing;o.immediateA=!o.immediateB&&ia!=Missing;
        if(o.immediateA)o.a=ia;else if(Reloaded(na)){o.a=LoadUniform(na);o.temps[o.count++]=o.a;}else o.a=registers[na];
        if(o.immediateB)o.b=ib;else if(nb==na)o.b=o.a;else if(Reloaded(nb)){o.b=LoadUniform(nb);o.temps[o.count++]=o.b;}else o.b=registers[nb];
        Need((o.immediateA?o.a<48:o.a<32)&&(o.immediateB?o.b<48:o.b<32),"invalid scalar register assignment");return o;};
    auto Release=[&](const Operands&o,uint32_t na,uint32_t nb,unsigned consumers){
        for(unsigned t=0;t<o.count;++t)occupied[o.temps[t]]=false;
        for(unsigned t=0;t<consumers;++t){if(!Reloaded(na)&&!--uses[na])occupied[registers[na]]=false;if(!Reloaded(nb)&&!--uses[nb])occupied[registers[nb]]=false;}
    };
    // Vertex outputs are stored to the VPM as soon as their value exists, which
    // frees the register. Small immediates stop at 15, so later VPM indices come
    // from a uniform loaded into a scratch register.
    std::vector<bool> outputWritten(roots.size());
    auto WriteOutputs=[&](){
        if(stage==ShaderStage::Pixel||loadedVertexAttributes<liveVertexAttributes)return;
        for(unsigned c=0;c<roots.size();++c)if(!outputWritten[c]&&(registers[roots[c]]!=Missing||Reloaded(roots[c]))){
            bool constant=Reloaded(roots[c]);unsigned value=constant?LoadUniform(roots[c]):registers[roots[c]];
            if(c<16){result.code.push_back(UINT64_C(0x39c02180be03f000)|(uint64_t(c)<<6)|value);result.code.push_back(Nop);}
            else{
                unsigned index=0;while(index<32&&occupied[index])++index;Need(index<32,"vertex output register pressure exceeded");
                uniformValue[index]=Missing;
                result.code.push_back(UINT64_C(0x39803186bb03f000)|(uint64_t(index)<<46));result.uniforms.push_back({UniformKind::Literal,c,0});result.code.push_back(Nop);
                result.code.push_back(UINT64_C(0x38002180be03f000)|(uint64_t(index)<<6)|value);result.code.push_back(Nop);
            }
            outputWritten[c]=true;if(constant||!--uses[roots[c]])occupied[value]=false;
        }
    };
    // Vertex attributes are loaded lazily as their first consumer is reached.
    // Input and output values share VPM storage, so WriteOutputs stays blocked
    // until every live input scalar has been read at least once. This avoids
    // pinning a 32-scalar vertex in all 32 QPU registers before uniform loads.
    WriteOutputs();
    bool targetRead=false;
    for (uint32_t i = 0; i < nodes.size(); ++i) if (live[i]) {
        if(nodes[i].op==Op::Uniform&&nodes[i].uniform.kind==UniformKind::TargetColor&&!targetRead){
            Need(stage==ShaderStage::Pixel,"target color is only available to pixel shaders");
            if(varyingScalars)LoadVaryings(varyingScalars-1);
            // All texture reads precede this final thread switch. Acquire the
            // tile scoreboard before reading the previous pixel color.
            result.code.insert(result.code.end(),{Switch,Switch,Nop});targetRead=true;
            for(unsigned c=0;c<4;++c){unsigned reg=0;while(reg<32&&occupied[reg])++reg;Need(reg<32,"target color register pressure exceeded");
                for(uint32_t n=i;n<nodes.size();++n)if(live[n]&&nodes[n].op==Op::Uniform&&nodes[n].uniform.kind==UniformKind::TargetColor&&nodes[n].uniform.value==c){occupied[reg]=true;registers[n]=reg;break;}
                uniformValue[reg]=Missing;
                result.code.push_back((c?UINT64_C(0x3a003186bb03f000):UINT64_C(0x3a203186bb03f000))|(uint64_t(reg)<<46));result.code.push_back(Nop);
            }
            result.uniforms.push_back({UniformKind::Literal,0xffffff3fu,0});
        }
        if(nodes[i].op==Op::Uniform)continue;
        Ensure(nodes[i].a);Ensure(nodes[i].b);
        if(nodes[i].op==Op::Select){
            const auto&n=nodes[i];Ensure(n.c);
            Operands values=Fetch(n.b,n.c);
            unsigned mask=n.a==n.b?values.a:n.a==n.c?values.b:Reloaded(n.a)?LoadUniform(n.a):registers[n.a];
            Need(mask<32,"invalid select mask register");
            unsigned reg=0;while(reg<32&&occupied[reg])++reg;
            Need(reg<32,"select register pressure exceeded");
            result.code.push_back(qpu::SelectFalse(reg,mask,values.b));
            result.code.push_back(Add(182,reg,values.a,values.a)|(UINT64_C(0x28)<<46));
            Release(values,n.b,n.c,1);
            if(Reloaded(n.a))occupied[mask]=false;else if(!--uses[n.a])occupied[mask]=false;
            occupied[reg]=true;registers[i]=reg;uniformValue[reg]=Missing;
            WriteOutputs();continue;
        }
        if(nodes[i].op==Op::Texture){
            Need(!targetRead,"texture lookup after tile scoreboard lock");
            const auto &n=nodes[i];unsigned group=n.uniform.slot;if(sampleEmitted[group])continue;sampleEmitted[group]=true;const auto &lookup=lookups[group];bool constant=lookup.kind==Lookup::Constant;
            if(constant){result.constantSlot=constantSlot;result.constantWords=constantWords;result.constantDeclaredWords=constantSizes[constantSlot]*4;}
            Operands coordinates=Fetch(n.a,n.b);unsigned destinations[4],a=coordinates.a,b=coordinates.b;
            for(unsigned c=0;c<lookup.words;++c)if(live[lookup.values[c]]){unsigned reg=0;while(reg<32&&occupied[reg])++reg;Need(reg<32,"texture register pressure exceeded");occupied[reg]=true;registers[lookup.values[c]]=destinations[c]=reg;}
            unsigned scratch=0;while(scratch<32&&occupied[scratch])++scratch;Need(scratch<32,"texture temporary register pressure exceeded");
            for(unsigned c=0;c<lookup.words;++c)if(!live[lookup.values[c]])destinations[c]=scratch;
            constexpr uint64_t Config=UINT64_C(0x3a403186bb03f000);uint32_t binding=constant?0:Remap(lookup.binding),token=binding<<PI5_BINDING_TOKEN_SHIFT;
            result.code.insert(result.code.end(),{Config,Config});
            result.uniforms.push_back({constant?UniformKind::ConstantTextureConfig:UniformKind::TextureConfig,constant?PI5_CONSTANT_TEXTURE_TOKEN:PI5_TEXTURE_TOKEN|token,binding});
            result.uniforms.push_back({constant?UniformKind::ConstantSamplerConfig:UniformKind::SamplerConfig,constant?PI5_CONSTANT_SAMPLER_TOKEN:PI5_SAMPLER_TOKEN|token,binding});
            if(lookup.kind!=Lookup::Fetch1D)result.code.push_back(Add(182,34,b,b,true));
            result.code.insert(result.code.end(),{Add(182,lookup.kind==Lookup::Sample||constant?33:41,a,a,true),Nop,Switch,Nop,Nop,Nop});
            for(unsigned c=0;c<lookup.words;++c){uniformValue[destinations[c]]=Missing;result.code.push_back(UINT64_C(0x38803186bb03f000)|(uint64_t(destinations[c])<<46));}
            unsigned consumers=0;for(unsigned c=0;c<lookup.words;++c)consumers+=live[lookup.values[c]];
            Release(coordinates,n.a,n.b,consumers);
            WriteOutputs();continue;
        }
        const Node &n = nodes[i];
        bool immediate=n.op==Op::Mul||n.op==Op::Add||n.op==Op::Sub||n.op==Op::Iadd||n.op==Op::Isub||n.op==Op::Min||n.op==Op::Max||
            n.op==Op::And||n.op==Op::Or||n.op==Op::Xor||n.op==Op::Shl||n.op==Op::Shr||n.op==Op::Asr||
            n.op==Op::Imin||n.op==Op::Imax||n.op==Op::Umin||n.op==Op::Umax;
        Operands operands = Fetch(n.a,n.b,immediate);
        unsigned reg = 0; while (reg < 32 && occupied[reg]) ++reg;
        bool comparison=n.op==Op::Feq||n.op==Op::Fne||n.op==Op::Flt||n.op==Op::Fge||n.op==Op::Ieq||n.op==Op::Ine||n.op==Op::Ilt||n.op==Op::Ult||n.op==Op::Fzeq||n.op==Op::Fzne;
        if(!comparison&&n.op!=Op::Ftz){if(!operands.immediateA&&((!Reloaded(n.a)&&uses[n.a]==(n.a==n.b?2u:1u))||(reg==32&&Reloaded(n.a))))reg=operands.a;
            else if(!operands.immediateB&&((!Reloaded(n.b)&&uses[n.b]==1)||(reg==32&&Reloaded(n.b))))reg=operands.b;}
        Need(reg < 32,"shader register pressure exceeds the 4-thread register bank");
        occupied[reg] = true; registers[i] = reg;uniformValue[reg]=Missing;
        {
            unsigned a = operands.a, b = operands.b;
            if(n.op==Op::Feq||n.op==Op::Fne||n.op==Op::Flt||n.op==Op::Fge||n.op==Op::Ieq||n.op==Op::Ine||n.op==Op::Ilt||n.op==Op::Ult||n.op==Op::Fzeq||n.op==Op::Fzne){
                // FCMP: Z for ordered equality, N for <, reversed C for >=.
                // Integer XOR/Z, reversed MIN/C and SUB/C give exact integer
                // comparisons without converting to floating point.
                unsigned flag=n.op==Op::Flt?2:(n.op==Op::Fge||n.op==Op::Ilt||n.op==Op::Ult)?3:1;
                unsigned operation=(n.op==Op::Fzeq||n.op==Op::Fzne)?181:(n.op==Op::Ieq||n.op==Op::Ine)?183:n.op==Op::Ilt?120:n.op==Op::Ult?60:197;
                if(n.op==Op::Fge||n.op==Op::Ilt)std::swap(a,b);
                result.code.push_back(qpu::FlagAndZero(operation,reg,a,b,flag));
                result.code.push_back(Add(186,reg,reg,0)|(uint64_t(n.op==Op::Fne||n.op==Op::Ine||n.op==Op::Fzne?0x28:0x20)<<46));
            }else if(n.op==Op::Ftz){
                // Inspect exponent bits without changing the source. Preserve
                // normal/Inf/NaN words, and write +0 for both signed subnormals.
                result.code.push_back(qpu::FlagAndZero(181,reg,a,b,1));
                result.code.push_back(Add(182,reg,a,a)|(UINT64_C(0x28)<<46));
            }else if (n.op == Op::Mul) {
                uint64_t word = Nop;
                word &= ~((UINT64_C(63) << 58) | (UINT64_C(1) << 45) | (UINT64_C(63) << 38) | (UINT64_C(4095) << 12));
                unsigned signal=operands.immediateA?30:operands.immediateB?31:0;
                Plain(word | (UINT64_C(21) << 58) | (uint64_t(reg) << 38) | (uint64_t(a) << 18) | (uint64_t(b) << 12) | (uint64_t(signal)<<53));
            } else if (n.op == Op::Imul) {
                // MULTOP loads the cross-product high bits into rtop, which the
                // following UMUL24 consumes. No thread switch may separate them.
                uint64_t word = Nop & ~((UINT64_C(63) << 58) | (UINT64_C(4095) << 12));
                result.code.push_back(word | (UINT64_C(10) << 58) | (uint64_t(a) << 18) | (uint64_t(b) << 12));
                result.code.push_back(Nop);
                word = Nop & ~((UINT64_C(63) << 58) | (UINT64_C(1) << 45) | (UINT64_C(63) << 38) | (UINT64_C(4095) << 12));
                result.code.push_back(word | (UINT64_C(3) << 58) | (uint64_t(reg) << 38) | (uint64_t(a) << 18) | (uint64_t(b) << 12));
            } else if (n.op == Op::Reciprocal || n.op == Op::Rsqrt || n.op == Op::Exp || n.op == Op::Log) {
                unsigned selector=n.op==Op::Reciprocal?32:n.op==Op::Rsqrt?33:n.op==Op::Exp?34:35;
                result.code.push_back(Add(188,reg,a,selector));
            } else if (n.op == Op::Round || n.op == Op::Trunc || n.op == Op::Floor || n.op == Op::Ceil || n.op == Op::Fdx || n.op == Op::Fdy) {
                // Selector: operation base, unpacked 32-bit input (bit 2), unpacked output.
                bool derivative = n.op == Op::Fdx || n.op == Op::Fdy;
                unsigned selector = n.op == Op::Round || n.op == Op::Fdx ? 4 : n.op == Op::Trunc || n.op == Op::Fdy ? 20 : n.op == Op::Floor ? 36 : 52;
                result.code.push_back(Add(derivative ? 246 : 245,reg,a,selector));
            } else if (n.op == Op::Itof || n.op == Op::Utof || n.op == Op::Ftoi || n.op == Op::Ftou || n.op == Op::Ftoin) {
                bool toFloat = n.op == Op::Itof || n.op == Op::Utof;
                unsigned selector = n.op == Op::Itof ? 32 : n.op == Op::Utof ? 36 : n.op == Op::Ftoi ? 23 : n.op == Op::Ftou ? 39 : 7;
                result.code.push_back(Add(toFloat ? 246 : 245,reg,a,selector));
            } else {
                unsigned op = n.op == Op::Add ? 5 : n.op == Op::Sub ? 69 : n.op == Op::Iadd ? 56 : n.op == Op::Isub ? 60 : n.op == Op::And ? 181 : n.op == Op::Or ? 182 : n.op == Op::Xor ? 183 :
                              n.op == Op::Imin ? 120 : n.op == Op::Imax ? 121 : n.op == Op::Umin ? 122 : n.op == Op::Umax ? 123 :
                              n.op == Op::Shl ? 124 : n.op == Op::Shr ? 125 : n.op == Op::Asr ? 126 : 133;
                bool ia=operands.immediateA,ib=operands.immediateB;unsigned keyA=a+(ia?256:0),keyB=b+(ib?256:0);
                if (((n.op == Op::Add || n.op == Op::Min) && keyA > keyB) || (n.op == Op::Max && keyA <= keyB)){std::swap(a,b);std::swap(ia,ib);}
                Plain(Add(op,reg,a,b)|(uint64_t(ia?14:ib?15:0)<<53));
            }
            Release(operands,n.a,n.b,1);occupied[reg]=true;uniformValue[reg]=Missing;
        }
        // V3D 7.1 register-file SFU results have a two-instruction latency.
        // Keep one unpaired gap; MULTOP/UMUL24 also retains its trailing gap.
        if (n.op == Op::Reciprocal || n.op == Op::Rsqrt || n.op == Op::Exp || n.op == Op::Log || n.op == Op::Imul) result.code.push_back(Nop);
        WriteOutputs();
    }
    if (stage == ShaderStage::Pixel && varyingScalars) LoadVaryings(varyingScalars - 1);
    if(stage!=ShaderStage::Pixel&&loadedVertexAttributes<liveVertexAttributes){
        // Inputs used only as final outputs may not have an ALU consumer.
        // Drain those remaining live VPM reads before permitting the first
        // output store; attributes with future ALU uses are still resident.
        for(uint32_t i=0;i<nodes.size();++i)
            if(live[i]&&nodes[i].op==Op::Uniform&&nodes[i].uniform.kind==UniformKind::VertexAttribute&&registers[i]==Missing)
                LoadVertexAttribute(i);
    }
    WriteOutputs();
    for (unsigned c = 0; c < roots.size(); ++c) Need(stage == ShaderStage::Pixel || outputWritten[c],"vertex output was not produced");
    std::vector<unsigned> colors;
    if (stage == ShaderStage::Pixel) for (auto root : roots) colors.push_back(Reloaded(root) ? LoadUniform(root) : registers[root]);
    if(!targetRead)result.code.insert(result.code.end(),{Switch,Switch,Nop});
    for (unsigned c = 0; c < colors.size(); ++c) result.code.push_back(Add(182,c ? 7 : 8,colors[c],colors[c],true));
    if (stage == ShaderStage::Pixel) result.uniforms.push_back({UniformKind::Literal,0xffffff3fu,0});
    result.code.insert(result.code.end(),{Nop,Switch,Nop,Nop});
    if(stage==ShaderStage::Pixel){
        // Pack only instructions explicitly emitted as plain arithmetic.
        // Varying interpolation, texture/scoreboard sequences and SFU delays
        // keep their exact layout. Move candidates across at most eight plain
        // instructions, preserving RAW, WAR, WAW and uniform FIFO dependencies.
        std::vector<bool> allowed(result.code.size());for(auto i:packable)allowed[i]=true;
        std::vector<uint64_t> packed;packed.reserve(result.code.size());
        for(size_t i=0;i<result.code.size();++i){
            auto first=qpu::Decode(result.code[i]);
            if(allowed[i]&&first.valid)for(size_t j=i+1;j<result.code.size()&&j<=i+8;){
                auto candidate=qpu::Decode(result.code[j]);if(!allowed[j]||!candidate.valid)break;
                bool ready=true;for(size_t k=i+1;k<j;++k){auto prior=qpu::Decode(result.code[k]);
                    if((candidate.writes&(prior.reads|prior.writes))||(candidate.reads&prior.writes)){ready=false;break;}}
                if(ready&&qpu::Merge(first,candidate)){result.code.erase(result.code.begin()+j);allowed.erase(allowed.begin()+j);continue;}
                ++j;
            }
            packed.push_back(first.word);
        }
        result.code=std::move(packed);
    }
    Need(result.code.size() <= PI5_MAX_PROGRAM_WORDS,"shader instruction limit exceeded");
    Need(result.uniforms.size() <= PI5_MAX_PROGRAM_UNIFORMS,"shader uniform limit exceeded");
    return result;
}
// Sharing values extends their lifetime. Reduce the reuse window on register
// pressure before falling back to entirely unshared lowering.
Shader RunCompiler(ShaderStage stage,const uint8_t*data,size_t words,const ShaderSignature*link=nullptr,const ConstantBuffers*constants=nullptr,const ShaderBlend*blend=nullptr){
    for(unsigned window:{64u,32u,16u,8u,0u}){
        try{Compiler compiler(stage,link,constants,blend,window);return compiler.Run(data,words);}
        catch(const Failure&e){if(!window||!std::strstr(e.what(),"register pressure"))throw;}
    }
    throw Failure("shader register allocation failed");
}
}

bool CompileShader(const void *dxbc, size_t bytes, ShaderStage stage, Shader &out, std::string &error,const ShaderSignature *linkedVaryings) {
    out = {}; error.clear();
    try {
        Need(dxbc && bytes >= 32 && bytes <= 1024 * 1024 && !(bytes & 3),"invalid DXBC container size");
        const auto *data = static_cast<const uint8_t *>(dxbc);
        Need(Word(data) == 0x43425844 && Word(data + 20) == 1 && Word(data + 24) == bytes,"invalid DXBC header");
        uint32_t chunks = Word(data + 28);
        Need(chunks && chunks <= 128 && chunks <= (bytes - 32) / 4,"invalid DXBC chunk table");
        const uint8_t *shader = nullptr; size_t shaderBytes = 0;
        std::vector<std::pair<uint32_t,uint32_t>> ranges;
        for (uint32_t i = 0; i < chunks; ++i) {
            uint32_t offset = Word(data + 32 + 4 * i);
            Need(!(offset & 3) && offset >= 32 + 4 * chunks && offset <= bytes - 8,"invalid DXBC chunk offset");
            uint32_t size = Word(data + offset + 4);
            Need(size <= bytes - offset - 8,"invalid DXBC chunk length");
            for (const auto &range : ranges) Need(offset + size + 8 <= range.first || offset >= range.second,"overlapping DXBC chunks");
            ranges.emplace_back(offset,offset + size + 8);
            uint32_t type = Word(data + offset);
            if (type == 0x52444853 || type == 0x58454853) {
                Need(!shader && size >= 8 && !(size & 3),"invalid DXBC shader chunk");
                shader = data + offset + 8; shaderBytes = size;
            }
        }
        Need(shader != nullptr,"missing DXBC shader chunk");
        out = RunCompiler(stage,shader,shaderBytes / 4,linkedVaryings); return true;
    } catch (const std::exception &e) { error = e.what(); out = {}; return false; }
}
bool CompilePixelShader(const void *dxbc, size_t bytes, Shader &out, std::string &error) {
    return CompileShader(dxbc,bytes,ShaderStage::Pixel,out,error);
}
bool CompileShaderTokens(const uint32_t *tokens, size_t words, ShaderStage stage, Shader &out, std::string &error,const ShaderSignature *linkedVaryings,const ConstantBuffers *constants,const ShaderBlend *blend) {
    out = {}; error.clear();
    try {
        Need(tokens && words >= 2 && words <= 65536,"invalid shader token count");
        out = RunCompiler(stage,reinterpret_cast<const uint8_t*>(tokens),words,linkedVaryings,constants,blend); return true;
    } catch (const std::exception &e) { error = e.what(); out = {}; return false; }
}
}
