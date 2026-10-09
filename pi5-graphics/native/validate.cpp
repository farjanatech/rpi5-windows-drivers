#include "validate.h"
#include "qpu-alu.h"
#include <string.h>
namespace pi5 {
namespace {
constexpr uint64_t Nop=UINT64_C(0x38003186bb03f000),Switch=UINT64_C(0x38203186bb03f000),Config=UINT64_C(0x3a403186bb03f000);
constexpr uint64_t MulBase=Nop&~((UINT64_C(63)<<58)|(UINT64_C(1)<<45)|(UINT64_C(63)<<38)|(UINT64_C(4095)<<12));
uint64_t Add(unsigned op,unsigned destination,unsigned a,unsigned b,bool magic=false){
    uint64_t word=Nop&~((UINT64_C(1)<<44)|(UINT64_C(63)<<32)|(UINT64_C(255)<<24)|UINT64_C(4095));
    return word|(uint64_t(magic)<<44)|(uint64_t(destination)<<32)|(uint64_t(op)<<24)|(uint64_t(a)<<6)|b;
}
uint64_t Mul(unsigned op,unsigned destination,unsigned a,unsigned b){return MulBase|(uint64_t(op)<<58)|(uint64_t(destination)<<38)|(uint64_t(a)<<18)|(uint64_t(b)<<12);}
uint64_t Multop(unsigned a,unsigned b){return (Nop&~((UINT64_C(63)<<58)|(UINT64_C(4095)<<12)))|(UINT64_C(10)<<58)|(uint64_t(a)<<18)|(uint64_t(b)<<12);}
bool Defined(uint32_t mask,unsigned reg){return reg<32&&(mask&(1u<<reg));}
bool ColorFormat(uint32_t format){return format==28||format==87||format==88;}
// Two-source add-ALU operations: fadd/fsub, add/sub, integer/unsigned min/max, shifts, fmin/fmax, and/or/xor.
bool BinaryOp(unsigned op){return qpu::Binary(op);}
}
bool ValidateProgram(const uint64_t *code,uint32_t words,const uint32_t *uniforms,uint32_t uniformCount,const ProgramRules &rules,TexturePatches *patches,uint32_t *registers,uint8_t *checkedUniforms,uint32_t *usedBindings){
    if(patches)*patches={};if(registers)*registers=0;if(usedBindings)*usedBindings=0;uint32_t samples=0,constantSamples=0,bindingsUsed=0,bindingsDeclared=0;
    const auto stage=rules.stage;const uint32_t varyingScalars=rules.varyingScalars,nonPerspectiveMask=rules.nonPerspectiveMask,flatMask=rules.flatMask;
    if(!code||!words||words>PI5_MAX_PROGRAM_WORDS||uniformCount>PI5_MAX_PROGRAM_UNIFORMS||(uniformCount&&!uniforms)||
       static_cast<uint32_t>(stage)>2||rules.vertexScalars>PI5_MAX_VERTEX_SCALARS||!Pi5ValidVaryingMasks(varyingScalars,nonPerspectiveMask,flatMask)||
       (stage!=ProgramStage::Pixel&&(nonPerspectiveMask||flatMask||rules.targetReads)))return false;
    for(unsigned b=0;b<PI5_BINDINGS;++b){if((rules.bindingKinds[b]&&rules.bindingKinds[b]!=Pi5BindingTexture&&!Pi5BufferElementBytes(rules.bindingKinds[b])))return false;if(rules.bindingKinds[b])bindingsDeclared|=1u<<b;}
    if(checkedUniforms)memset(checkedUniforms,0,uniformCount);
    auto CheckedUniform=[&](uint32_t index){if(checkedUniforms)checkedUniforms[index]=1;return uniforms[index];};
    uint32_t at=0,used=0,defined=0,varyings=0,written=0;
    auto Padding=[&](){while(at<words&&code[at]==Nop)++at;};
    const unsigned outputs=stage==ProgramStage::Coordinate?6:stage==ProgramStage::Vertex?4+varyingScalars:4;
    if(stage==ProgramStage::Pixel&&varyingScalars)defined|=(1u<<0)|(1u<<3);
    // Vertex/coordinate VPM stores: index 0-15 as a small immediate, or a uniform
    // loaded into a scratch register immediately before a register-indexed store.
    // Each output index is stored exactly once. Returns 1 if consumed, 0 if not a store, -1 if invalid.
    auto VpmStore=[&]()->int{
        if(stage==ProgramStage::Pixel)return 0;
        uint64_t word=code[at];
        if((word&~UINT64_C(4095))==UINT64_C(0x39c02180be03f000)){
            unsigned index=unsigned((word>>6)&63),reg=unsigned(word&63);
            if(index>=16||index>=outputs||(written&(1u<<index))||!Defined(defined,reg)||words-at<2||code[at+1]!=Nop)return -1;
            written|=1u<<index;at+=2;return 1;
        }
        if((word&~(UINT64_C(31)<<46))!=UINT64_C(0x39803186bb03f000)||words-at<4||code[at+1]!=Nop)return 0;
        unsigned scratch=unsigned((word>>46)&31);uint64_t store=code[at+2];unsigned reg=unsigned(store&63);
        if(store!=(UINT64_C(0x38002180be03f000)|(uint64_t(scratch)<<6)|reg))return 0;
        if(used>=uniformCount)return -1;uint32_t index=CheckedUniform(used);
        if(index<16||index>=outputs||(written&(1u<<index))||reg==scratch||!Defined(defined,reg)||code[at+3]!=Nop)return -1;
        ++used;defined|=1u<<scratch;written|=1u<<index;at+=4;return 1;
    };
    bool targetRead=false;
    while(at<words){
        if(code[at]==Switch){
            if(words-at<4||code[at+1]!=Switch||code[at+2]!=Nop||
               (code[at+3]&~(UINT64_C(31)<<46))!=UINT64_C(0x3a203186bb03f000))break;
            if(stage!=ProgramStage::Pixel||!rules.targetReads||targetRead||varyings!=varyingScalars||words-at<11||used>=uniformCount||CheckedUniform(used++)!=0xffffff3fu)return false;
            targetRead=true;at+=3;
            for(unsigned c=0;c<4;++c){uint64_t word=code[at++];unsigned reg=unsigned((word>>46)&31);
                if(word!=((c?UINT64_C(0x3a003186bb03f000):UINT64_C(0x3a203186bb03f000))|(uint64_t(reg)<<46))||code[at++]!=Nop)return false;
                defined|=1u<<reg;
            }
            continue;
        }
        // Texture requests cannot run while holding the tile scoreboard lock.
        if(targetRead&&code[at]==Add(182,8,unsigned(code[at]&63),unsigned(code[at]&63),true))break;
        if(int store=VpmStore()){if(store<0)return false;continue;}
        // Varying loads may appear anywhere in the body but must be in order.
        if((code[at]&~(UINT64_C(31)<<46))==UINT64_C(0x39003186bb03f000)){
            if(stage!=ProgramStage::Pixel||varyings>=varyingScalars||words-at<3)return false;
            bool flat=(flatMask&(1u<<varyings))!=0;
            uint64_t load=code[at++];unsigned reg=unsigned((load>>46)&31);
            if(!reg||reg==3)return false;
            bool gap=at<words&&code[at]==Nop;if(gap)++at;
            if(!flat&&!(nonPerspectiveMask&(1u<<varyings))){if(at>=words||code[at++]!=Mul(21,reg,reg,3))return false;}else if(!gap)return false;
            Padding();
            if(at>=words||code[at++]!=(flat?Add(182,reg,0,0):Add(5,reg,0,reg)))return false;
            Padding();
            defined|=1u<<reg;++varyings;continue;
        }
        if(code[at]==Config){
            if(targetRead||samples>=PI5_MAX_LOOKUPS||words-at<8||uniformCount-used<2)return false;
            uint32_t texture=CheckedUniform(used),sampler=CheckedUniform(used+1),binding=(texture>>PI5_BINDING_TOKEN_SHIFT)&15;
            bool constant=texture==PI5_CONSTANT_TEXTURE_TOKEN&&sampler==PI5_CONSTANT_SAMPLER_TOKEN;uint32_t kind=0;
            if(constant){if(!rules.constants)return false;++constantSamples;binding=0;}
            else{if(binding>=PI5_BINDINGS||texture!=(PI5_TEXTURE_TOKEN|(binding<<PI5_BINDING_TOKEN_SHIFT))||sampler!=(PI5_SAMPLER_TOKEN|(binding<<PI5_BINDING_TOKEN_SHIFT)))return false;
                kind=rules.bindingKinds[binding];if(!kind||(stage!=ProgramStage::Pixel&&kind==Pi5BindingTexture))return false;bindingsUsed|=1u<<binding;}
            ++at;Padding();if(at>=words||code[at++]!=Config)return false;Padding();
            if(at>=words)return false;bool second=false;uint64_t word=code[at];unsigned reg=unsigned(word&63);
            if(word==Add(182,34,reg,reg,true)){if(!Defined(defined,reg))return false;second=true;++at;Padding();}
            if(words-at<5)return false;word=code[at];reg=unsigned(word&63);
            unsigned retiring=unsigned((word>>32)&63);
            if((retiring!=33&&retiring!=41)||word!=Add(182,retiring,reg,reg,true)||!Defined(defined,reg))return false;++at;Padding();
            LookupForm form=retiring==33?LookupForm::Sample:second?LookupForm::Fetch2D:LookupForm::Fetch1D;
            if(retiring==33&&!second)return false;
            if(constant?form!=LookupForm::Sample:kind==Pi5BindingTexture?form==LookupForm::Fetch1D:form!=LookupForm::Fetch1D)return false;
            unsigned results=4;
            if(words-at<4||code[at++]!=Switch||code[at++]!=Nop||code[at++]!=Nop||code[at++]!=Nop||words-at<results)return false;
            if(patches){auto&p=patches->lookups[samples];p.config0=used;p.config1=used+1;p.binding=binding;p.form=form;p.constant=constant;patches->count=samples+1;}++samples;used+=2;
            for(unsigned i=0;i<results;++i){if(at>=words)return false;word=code[at++];reg=unsigned((word>>46)&31);if(word!=(UINT64_C(0x38803186bb03f000)|(uint64_t(reg)<<46)))return false;defined|=1u<<reg;Padding();}
            continue;
        }
        // A bounded immediate MOV reads the fixed 48-entry hardware table;
        // its source field is not a register or an address-bearing signal.
        {uint64_t word=code[at];unsigned dst=unsigned((word>>32)&63),immediate=unsigned((word>>6)&63);
            if(dst<32&&immediate<48&&word==(Add(249,dst,immediate,3)|(UINT64_C(14)<<53))){defined|=1u<<dst;++at;continue;}
        }
        // Paired ALUs and an optional uniform load read the register state
        // before any of their distinct destinations are updated.
        {auto alu=qpu::Decode(code[at]);
            if(alu.valid&&alu.add+alu.mul+unsigned(alu.signal==12)>1){
                if(uint32_t(alu.reads)&~defined)return false;
                if(alu.signal==12){if(used>=uniformCount)return false;++used;}
                defined|=uint32_t(alu.writes);++at;continue;
            }
        }
        // One source may use the fixed small-immediate table. Admit only
        // unpredicated scalar ALU forms, with no extra signals or magic writes.
        {uint64_t word=code[at];unsigned signal=unsigned((word>>53)&31);
            if(signal==14||signal==15){unsigned op=unsigned((word>>24)&255),dst=unsigned((word>>32)&63),a=unsigned((word>>6)&63),b=unsigned(word&63);
                if(dst<32&&BinaryOp(op)&&word==(Add(op,dst,a,b)|(uint64_t(signal)<<53))){
                    if(signal==14?(a>=48||!Defined(defined,b)):(b>=48||!Defined(defined,a)))return false;
                    defined|=1u<<dst;++at;continue;
                }
            }
            if(signal==30||signal==31){unsigned dst=unsigned((word>>38)&63),a=unsigned((word>>18)&63),b=unsigned((word>>12)&63);
                if(dst>=32||word!=(Mul(21,dst,a,b)|(uint64_t(signal)<<53))||
                    (signal==30?(a>=48||!Defined(defined,b)):(b>=48||!Defined(defined,a))))return false;
                defined|=1u<<dst;++at;continue;
            }
        }
        // Bounded, adjacent conditional copy. Every input is defined before
        // the first instruction and the destination cannot clobber true data.
        {uint64_t word=code[at];unsigned dst=unsigned((word>>38)&63),mask=unsigned((word>>6)&63),value=unsigned((word>>18)&63);
            if(dst<32&&word==qpu::SelectFalse(dst,mask,value)){
                if(!Defined(defined,mask)||!Defined(defined,value)||words-at<2)return false;
                uint64_t last=code[at+1];unsigned source=unsigned(last&63);
                if(dst==source||!Defined(defined,source)||last!=(Add(182,dst,source,source)|(UINT64_C(0x28)<<46)))return false;
                defined|=1u<<dst;at+=2;continue;
            }
        }
        // A paired flag test and integer zero is followed immediately by its
        // sole conditional consumer. No arbitrary flag/peripheral form enters.
        {uint64_t word=code[at];unsigned dst=unsigned((word>>38)&63),a=unsigned((word>>6)&63),b=unsigned(word&63),flag=unsigned((word>>46)&127),op=unsigned((word>>24)&255);
            bool normalize=op==181&&flag==1;
            bool compare=(op==197&&flag>=1&&flag<=3)||(op==183&&flag==1)||((op==120||op==60)&&flag==3);
            if(dst<32&&(normalize||compare)&&word==qpu::FlagAndZero(op,dst,a,b,flag)){
                if(!Defined(defined,a)||!Defined(defined,b)||dst==a||dst==b||words-at<2)return false;
                uint64_t last=code[at+1];
                bool copy=normalize&&last==(Add(182,dst,a,a)|(UINT64_C(0x28)<<46));
                bool mask=last==(Add(186,dst,dst,0)|(UINT64_C(0x20)<<46))||last==(Add(186,dst,dst,0)|(UINT64_C(0x28)<<46));
                if(!copy&&!mask)return false;
                defined|=1u<<dst;at+=2;continue;
            }
        }
        // Float comparison normalization: test exponent bits, clear the
        // result, then copy the original word only when the exponent is nonzero.
        {uint64_t word=code[at];unsigned dst=unsigned((word>>32)&63),a=unsigned((word>>6)&63),b=unsigned(word&63);
            if(dst<32&&word==(Add(181,dst,a,b)|(UINT64_C(1)<<46))){
                if(!Defined(defined,a)||!Defined(defined,b)||dst==a||dst==b||words-at<3||
                   code[at+1]!=Add(183,dst,dst,dst)||code[at+2]!=(Add(182,dst,a,a)|(UINT64_C(0x28)<<46)))return false;
                defined|=1u<<dst;at+=3;continue;
            }
        }
        // Comparisons are an indivisible three-instruction form: the ALU sets
        // flags, XOR initializes the mask, and a conditional NOT writes true.
        // Accept no independent flag update or conditional instruction.
        {uint64_t word=code[at];unsigned dst=unsigned((word>>32)&63),a=unsigned((word>>6)&63),b=unsigned(word&63),flag=unsigned((word>>46)&127),operation=unsigned((word>>24)&255);
            if(dst<32&&((operation==197&&flag>=1&&flag<=3)||(operation==183&&flag==1)||((operation==120||operation==60)&&flag==3))&&word==(Add(operation,dst,a,b)|(uint64_t(flag)<<46))){
                if(!Defined(defined,a)||!Defined(defined,b)||dst==a||dst==b||words-at<3||code[at+1]!=Add(183,dst,a,a))return false;
                uint64_t last=code[at+2];if(last!=(Add(186,dst,dst,0)|(UINT64_C(0x20)<<46))&&last!=(Add(186,dst,dst,0)|(UINT64_C(0x28)<<46)))return false;
                defined|=1u<<dst;at+=3;continue;
            }
        }
        uint64_t word=code[at++];unsigned reg=0,nops=0;bool writes=true;
        if(word==Nop)continue;
        if((word&~(UINT64_C(31)<<46))==UINT64_C(0x39803186bb03f000)){
            reg=unsigned((word>>46)&31);if(used>=uniformCount)return false;++used;
        }else if((word&~((UINT64_C(31)<<32)|(UINT64_C(63)<<6)))==UINT64_C(0x39c02180bc03f000)){
            nops=1;
            reg=unsigned((word>>32)&31);if(stage==ProgramStage::Pixel||((word>>6)&63)>=rules.vertexScalars)return false;
        }else{
            unsigned op=unsigned((word>>24)&255),a=unsigned((word>>6)&63),b=unsigned(word&63);
            reg=unsigned((word>>32)&63);
            // Register-file SFU results require one intervening instruction.
            nops=0;
            if(reg<32&&word==Add(op,reg,a,b)){
                if(!Defined(defined,a))return false;
                if(BinaryOp(op)){if(!Defined(defined,b))return false;}
                else if(op==188){if(b!=32&&b!=33)return false;nops=1;}
                else if(op==246){if(b!=32&&b!=36&&b!=4&&b!=20)return false;if((b==4||b==20)&&stage!=ProgramStage::Pixel)return false;}
                else if(op==245){if(b!=7&&b!=23&&b!=39&&b!=4&&b!=20&&b!=36&&b!=52)return false;}
                else return false;
            }else{
                unsigned mulOp=unsigned((word>>58)&63);reg=unsigned((word>>38)&63);a=unsigned((word>>18)&63);b=unsigned((word>>12)&63);
                if(!Defined(defined,a)||!Defined(defined,b))return false;
                if(mulOp==10){if(word!=Multop(a,b))return false;writes=false;nops=1;}
                else if((mulOp!=21&&mulOp!=3)||reg>=32||word!=Mul(mulOp,reg,a,b))return false;
            }
        }
        if(writes)defined|=1u<<reg;
        if(nops>words-at)return false;
        for(unsigned i=0;i<nops;++i)if(code[at++]!=Nop)return false;
    }
    if(stage==ProgramStage::Pixel&&varyings!=varyingScalars)return false;
    if(!targetRead&&(words-at<3||code[at++]!=Switch||code[at++]!=Switch||code[at++]!=Nop))return false;
    if(stage==ProgramStage::Pixel){
        for(unsigned i=0;i<outputs;++i){if(at>=words)return false;uint64_t word=code[at++];unsigned reg=unsigned(word&63);if(!Defined(defined,reg)||word!=Add(182,i?7:8,reg,reg,true))return false;}
    }else{
        while(at<words)if(int store=VpmStore()){if(store<0)return false;}else break;
        if(written!=(1u<<outputs)-1)return false;
    }
    if(stage==ProgramStage::Pixel){if(used>=uniformCount||CheckedUniform(used++)!=0xffffff3fu)return false;}
    bool valid=!(bindingsUsed&~bindingsDeclared)&&bool(constantSamples)==rules.constants&&used==uniformCount&&words-at==4&&code[at]==Nop&&code[at+1]==Switch&&code[at+2]==Nop&&code[at+3]==Nop;
    if(valid&&registers)*registers=defined;if(valid&&usedBindings)*usedBindings=bindingsUsed;
    return valid;
}
bool ProgramValidationCache::Validate(const uint64_t *program,uint32_t words,const uint32_t *uniforms,uint32_t uniformCount,
                                     const ProgramRules &rules,TexturePatches *patches,uint32_t *registers,uint32_t *usedBindings){
    if(!program||!words||words>Words||uniformCount>Uniforms||(uniformCount&&!uniforms))
        return ValidateProgram(program,words,uniforms,uniformCount,rules,patches,registers,nullptr,usedBindings);
    uint64_t hash=UINT64_C(14695981039346656037);
    for(uint32_t i=0;i<words;++i)hash=(hash^program[i])*UINT64_C(1099511628211);
    auto SameRules=[&](const ProgramRules&a){return a.stage==rules.stage&&a.vertexScalars==rules.vertexScalars&&
        a.varyingScalars==rules.varyingScalars&&a.nonPerspectiveMask==rules.nonPerspectiveMask&&a.flatMask==rules.flatMask&&
        a.constants==rules.constants&&a.targetReads==rules.targetReads&&!memcmp(a.bindingKinds,rules.bindingKinds,sizeof(a.bindingKinds));};
    uint32_t replace=0;
    for(uint32_t i=0;i<Entries;++i){auto&e=entries[i];
        if(e.words==words&&e.uniforms==uniformCount&&e.hash==hash&&SameRules(e.rules)&&
           !memcmp(e.code,program,words*8)){
            bool same=true;for(uint32_t u=0;u<uniformCount;++u)if(e.checked[u]&&e.values[u]!=uniforms[u]){same=false;break;}
            if(!same){if(e.age<entries[replace].age)replace=i;continue;}
            e.age=++age;if(patches)*patches=e.patches;if(registers)*registers=e.registers;if(usedBindings)*usedBindings=e.bindings;return true;}
        if(e.age<entries[replace].age)replace=i;
    }
    auto&e=entries[replace];e.words=0;
    if(!ValidateProgram(program,words,uniforms,uniformCount,rules,&e.patches,&e.registers,e.checked,&e.bindings)){
        if(patches)*patches={};if(registers)*registers=0;if(usedBindings)*usedBindings=0;return false;}
    memcpy(e.code,program,words*8);if(uniformCount)memcpy(e.values,uniforms,uniformCount*4);
    e.rules=rules;e.hash=hash;e.uniforms=uniformCount;e.age=++age;e.words=words;
    if(patches)*patches=e.patches;if(registers)*registers=e.registers;if(usedBindings)*usedBindings=e.bindings;return true;
}
ProgramRules DrawProgramRules(const Pi5DrawCommand &c,unsigned i){
    ProgramRules rules;const Pi5Program *programs[]={&c.Coordinate,&c.Vertex,&c.Pixel};
    rules.stage=i==0?ProgramStage::Coordinate:i==1?ProgramStage::Vertex:ProgramStage::Pixel;rules.vertexScalars=c.VertexComponents;rules.varyingScalars=c.VaryingScalars;
    rules.constants=i<3&&programs[i]->ConstantWords!=0;
    if(i==2){rules.targetReads=(c.Pipeline.Flags&PI5_PIPELINE_SHADER_BLEND)!=0;rules.nonPerspectiveMask=c.NonPerspectiveMask;rules.flatMask=c.FlatMask;}
    for(uint32_t b=0;b<PI5_BINDINGS&&b<c.BindingCount;++b)rules.bindingKinds[b]=c.Bindings[b].Kind;
    return rules;
}
bool ValidateAllocation(const Pi5AllocationInfo &r){
    if((r.RefreshNumerator||r.RefreshDenominator)&&(!(r.MiscFlags&PI5_RESOURCE_DISPLAYABLE)||!r.RefreshDenominator))return false;
    if((r.MiscFlags&~(0x10au|PI5_RESOURCE_DISPLAYABLE))||(r.MiscFlags&&r.Dimension!=3)||r.Reserved||(r.PrivateFlags&~PI5_ALLOCATION_GPU_ONLY)||(r.PrivateFlags&&r.Dimension!=3))return false;
    if((r.MiscFlags&PI5_RESOURCE_DISPLAYABLE)&&(!(r.BindFlags&0x80u)||(r.Format!=87&&r.Format!=88)||r.Levels!=1))return false;
    if(r.Version!=PI5_UMD_ABI||!r.Bytes||r.Bytes>PI5_UMD_RESOURCE_BYTES||(r.Bytes&4095)||!r.Width||!r.Height||!r.Pitch||uint64_t(r.Height)*r.Pitch>r.Bytes)return false;
    if(r.Dimension==1)return r.Format==0&&r.Height==1&&r.Pitch==r.Width&&r.Levels==1;
    if(r.Dimension!=3||!ColorFormat(r.Format)||r.Width>4096||r.Height>4096||r.Pitch<r.Width*4||r.Pitch>16384||(r.Pitch&63))return false;
    uint32_t largest=r.Width>r.Height?r.Width:r.Height,levels=1;while(largest>>levels)++levels;
    return r.Levels>=1&&r.Levels<=levels&&Pi5LevelChain(r.Width,r.Height,r.Pitch,r.Levels,0,nullptr)<=r.Bytes;
}
bool ValidateCommand(const void *buffer,uint32_t bytes,const Pi5AllocationInfo *r,uint32_t count,ProgramValidationCache *cache){
    if(!buffer||(reinterpret_cast<uintptr_t>(buffer)&7)||bytes<sizeof(Pi5CommandHeader)||bytes>PI5_UMD_COMMAND_BYTES||!r||!count||count>PI5_MAX_REFERENCES)return false;
    auto data=static_cast<const uint8_t*>(buffer);auto h=static_cast<const Pi5CommandHeader*>(buffer);
    if(h->Magic!=PI5_UMD_MAGIC||h->Version!=PI5_UMD_ABI||h->Bytes!=bytes)return false;
    for(uint32_t i=0;i<count;++i)if(!ValidateAllocation(r[i]))return false;
    if(h->Operation==Pi5DrawBatch){
        if(bytes<sizeof(Pi5BatchCommand)||count<2)return false;
        auto batch=static_cast<const Pi5BatchCommand*>(buffer);
        if(!batch->Count||batch->Count>PI5_MAX_BATCH_DRAWS||batch->Reserved[0]||batch->Reserved[1]||batch->Reserved[2])return false;
        uint32_t seen=0,end=sizeof(*batch);
        for(uint32_t i=0;i<PI5_MAX_BATCH_DRAWS;++i){const auto&e=batch->Entries[i];
            if(i>=batch->Count){static const Pi5BatchEntry zero={};if(memcmp(&e,&zero,sizeof(e)))return false;continue;}
            if(e.Reserved||e.Count<2||e.Count>PI5_MAX_ALLOCATIONS||e.References[0]||e.References[1]!=1||
               (e.Offset&7)||e.Offset<end||e.Offset>bytes||e.Bytes<sizeof(Pi5DrawCommand)||e.Bytes>bytes-e.Offset)return false;
            for(uint32_t pad=end;pad<e.Offset;++pad)if(data[pad])return false;
            Pi5AllocationInfo local[PI5_MAX_ALLOCATIONS]={};uint32_t childSeen=0;
            for(uint32_t j=0;j<PI5_MAX_ALLOCATIONS;++j){uint32_t index=e.References[j];
                if(j>=e.Count){if(index)return false;continue;}
                if(index>=count||(j>=2&&index<2)||(childSeen&(1u<<index)))return false;
                childSeen|=1u<<index;local[j]=r[index];
            }
            // Reject nested batches before recursive validation; recursion is bounded to one level.
            auto child=reinterpret_cast<const Pi5CommandHeader*>(data+e.Offset);
            if(child->Operation!=Pi5Draw||!ValidateCommand(child,e.Bytes,local,e.Count,cache))return false;
            seen|=childSeen;end=e.Offset+e.Bytes;
        }
        return end==bytes&&seen==(UINT32_MAX>>(32-count));
    }
    if(count>PI5_MAX_ALLOCATIONS)return false;
    if(h->Operation==Pi5Clear){
        if(bytes!=sizeof(Pi5ClearCommand)||count!=1||r[0].Dimension!=3)return false;
        auto c=static_cast<const Pi5ClearCommand*>(buffer);return !c->Target&&!c->Reserved[0]&&!c->Reserved[1];
    }
    if(h->Operation==Pi5Copy){
        if(bytes!=sizeof(Pi5CopyCommand)||count!=2||r[0].Dimension!=3||r[1].Dimension!=3)return false;
        auto c=static_cast<const Pi5CopyCommand*>(buffer);
        return !c->Destination&&c->Source==1&&!c->Reserved[0]&&!c->Reserved[1]&&r[0].Width==r[1].Width&&r[0].Height==r[1].Height&&r[0].Pitch==r[1].Pitch&&r[0].Format==r[1].Format&&r[0].Levels==r[1].Levels;
    }
    if(h->Operation==Pi5CopyRegion){
        if(bytes!=sizeof(Pi5RegionCommand)||count!=2||r[0].Dimension!=r[1].Dimension||r[0].Format!=r[1].Format)return false;
        auto c=static_cast<const Pi5RegionCommand*>(buffer);Pi5Level dst={},src={};
        if(c->Destination||c->Source!=1||c->Reserved[0]||c->Reserved[1]||!Pi5AllocationLevel(r[0],c->DestinationLevel,dst)||!Pi5AllocationLevel(r[1],c->SourceLevel,src))return false;
        return c->Width&&c->Height&&c->SourceX<=src.Width&&c->Width<=src.Width-c->SourceX&&
            c->SourceY<=src.Height&&c->Height<=src.Height-c->SourceY&&c->DestinationX<=dst.Width&&c->Width<=dst.Width-c->DestinationX&&
            c->DestinationY<=dst.Height&&c->Height<=dst.Height-c->DestinationY;
    }
    if(h->Operation!=Pi5Draw||bytes<sizeof(Pi5DrawCommand)||count<2||r[0].Dimension!=3||r[1].Dimension!=1)return false;
    auto c=static_cast<const Pi5DrawCommand*>(buffer);
    const auto &state=c->Pipeline;
    if((state.Flags&~63u)||((state.Flags&1)&&(state.Flags&PI5_PIPELINE_SHADER_BLEND))||state.CullMode<1||state.CullMode>3||state.ColorMask>15||state.Reserved)return false;
    if(!Pi5ValidCoverage(state,r[0].Width,r[0].Height))return false;
    for(unsigned i=0;i<6;++i)if(state.Blend[i]>(i%3==2?4u:14u))return false;
    for(auto factor:state.Constant)if(factor>0x3c00)return false;
    if(state.Flags&8){if(state.Scissor[0]>=state.Scissor[2]||state.Scissor[1]>=state.Scissor[3]||state.Scissor[2]>r[0].Width||state.Scissor[3]>r[0].Height)return false;}
    else for(auto value:state.Scissor)if(value)return false;
    if(c->BindingCount>PI5_BINDINGS)return false;
    uint32_t referenced=0;
    for(uint32_t b=0;b<PI5_BINDINGS;++b){const auto&x=c->Bindings[b];
        if(b>=c->BindingCount){static const Pi5Binding none={};if(memcmp(&x,&none,sizeof(x)))return false;continue;}
        if(x.Allocation<2||x.Allocation>=count)return false;referenced|=1u<<x.Allocation;const auto&a=r[x.Allocation];
        if(x.Kind==Pi5BindingTexture){
            if(a.Dimension!=3||!x.Count||x.First>=a.Levels||x.Count>a.Levels-x.First||x.Filter>7||!Pi5ValidAddressModes(x.AddressModes)||x.MinLod>x.MaxLod||x.MaxLod>(15u<<8))return false;
            for(auto component:x.Border)if(component>0x3f800000u||(!Pi5UsesBorder(x.AddressModes)&&component))return false;
        }else if(uint32_t elementBytes=Pi5BufferElementBytes(x.Kind)){
            uint32_t elements=a.Width/elementBytes;
            if(a.Dimension!=1||!x.Count||x.Count>=PI5_MAX_BYTE_ELEMENTS||x.First>elements||x.Count>elements-x.First||x.Filter||x.AddressModes||x.MinLod||x.MaxLod)return false;
            for(auto component:x.Border)if(component)return false;
        }else return false;
    }
    if(referenced!=(((1u<<count)-1)&~3u))return false;
    if(c->Target||c->Vertices!=1||!c->VertexComponents||c->VertexComponents>PI5_MAX_VERTEX_SCALARS||!Pi5ValidVaryingMasks(c->VaryingScalars,c->NonPerspectiveMask,c->FlatMask)||
       !c->VertexCount||c->VertexCount>4095||c->VertexCount%3||c->VertexStride<c->VertexComponents*4||c->VertexStride>4096||((c->VertexOffset|c->VertexStride)&3)||
       c->VertexOffset>=r[1].Width||uint64_t(c->VertexCount-1)*c->VertexStride+c->VertexComponents*4>r[1].Width-c->VertexOffset)return false;
    uint32_t viewport[6],fixed[4];memcpy(viewport,c->Viewport,sizeof(viewport));
    if(!Pi5ViewportRect(viewport,r[0].Width,r[0].Height,fixed)||viewport[4]||viewport[5]!=0x3f800000)return false;
    const Pi5Program *programs[]={&c->Coordinate,&c->Vertex,&c->Pixel};uint32_t starts[9]={},ends[9]={},usedBindings=0;
    for(unsigned i=0;i<3;++i){const auto&p=*programs[i];
        if(!p.CodeCount||p.CodeCount>PI5_MAX_PROGRAM_WORDS||p.UniformCount>PI5_MAX_PROGRAM_UNIFORMS||(p.CodeOffset&7)||(p.UniformOffset&3)||p.CodeOffset<sizeof(*c)||p.UniformOffset<sizeof(*c)||p.CodeOffset>bytes||p.CodeCount*8>bytes-p.CodeOffset||p.UniformOffset>bytes||p.UniformCount*4>bytes-p.UniformOffset)return false;
        starts[i*2]=p.CodeOffset;ends[i*2]=p.CodeOffset+p.CodeCount*8;starts[i*2+1]=p.UniformOffset;ends[i*2+1]=p.UniformOffset+p.UniformCount*4;
        if(p.ConstantWords){if(p.ConstantWords<4||p.ConstantWords>4096||(p.ConstantWords&(p.ConstantWords-1))||(p.ConstantOffset&3)||p.ConstantOffset<sizeof(*c)||p.ConstantOffset>bytes||p.ConstantWords*4>bytes-p.ConstantOffset)return false;starts[6+i]=p.ConstantOffset;ends[6+i]=p.ConstantOffset+p.ConstantWords*4;}
        else if(p.ConstantOffset)return false;
        auto code=reinterpret_cast<const uint64_t*>(data+p.CodeOffset);auto uniforms=reinterpret_cast<const uint32_t*>(data+p.UniformOffset);auto rules=DrawProgramRules(*c,i);
        uint32_t bindings=0;
        if(!(cache?cache->Validate(code,p.CodeCount,uniforms,p.UniformCount,rules,nullptr,nullptr,&bindings):ValidateProgram(code,p.CodeCount,uniforms,p.UniformCount,rules,nullptr,nullptr,nullptr,&bindings)))return false;
        usedBindings|=bindings;
    }
    if(usedBindings!=(1u<<c->BindingCount)-1)return false;
    for(unsigned i=0;i<9;++i)for(unsigned j=0;j<i;++j)if(starts[i]<ends[j]&&starts[j]<ends[i])return false;
    return true;
}
}
