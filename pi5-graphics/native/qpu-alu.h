#pragma once
#include <stdint.h>

namespace pi5 {
namespace qpu {
constexpr uint64_t Nop=UINT64_C(0x38003186bb03f000);
constexpr uint64_t AddMask=(UINT64_C(1)<<44)|(UINT64_C(63)<<32)|(UINT64_C(255)<<24)|4095;
constexpr uint64_t MulMask=(UINT64_C(63)<<58)|(UINT64_C(1)<<45)|(UINT64_C(63)<<38)|(UINT64_C(4095)<<12);
constexpr uint64_t SignalMask=UINT64_C(31)<<53,FlagsMask=UINT64_C(127)<<46;
// Uniform FIFO ordering is tracked separately from the 64 physical-register
// dependency mask so RF32-RF63 remain available in 2-thread mode.
// Set add-ALU flags without storing its result, while integer SUB on the
// multiply ALU initializes a separate mask register to zero.
inline uint64_t FlagAndZero(unsigned op,unsigned dst,unsigned a,unsigned b,unsigned flag){
    return (Nop&~(AddMask|MulMask|FlagsMask))|(UINT64_C(1)<<44)|(UINT64_C(6)<<32)|
        (uint64_t(op)<<24)|(uint64_t(a)<<6)|b|(UINT64_C(2)<<58)|(uint64_t(dst)<<38)|
        (uint64_t(a)<<18)|(uint64_t(a)<<12)|(uint64_t(flag)<<46);
}
// Test a mask while the independent integer MOV initializes false lanes.
// MOV's unpack selector 3 preserves every bit, including NaNs/subnormals.
inline uint64_t SelectFalse(unsigned dst,unsigned mask,unsigned value){
    return (Nop&~(AddMask|MulMask|FlagsMask))|(UINT64_C(1)<<44)|(UINT64_C(6)<<32)|
        (UINT64_C(182)<<24)|(uint64_t(mask)<<6)|mask|(UINT64_C(14)<<58)|
        (uint64_t(dst)<<38)|(uint64_t(value)<<18)|(UINT64_C(3)<<12)|(UINT64_C(1)<<46);
}
struct Alu {
    uint64_t word,reads,writes;
    unsigned add,mul,signal;
    bool valid,uniformFifo;
};
inline bool Binary(unsigned op){return op==5||op==69||op==56||op==60||(op>=120&&op<=126)||op==133||op==181||op==182||op==183;}
// Only flag-free arithmetic, bounded small immediates and RF uniform loads.
// Branches, peripheral writes, thread switches and latency-sensitive operations
// are outside this form and remain explicit scheduling barriers.
inline Alu Decode(uint64_t word){
    Alu v={word,0,0,0,0,unsigned((word>>53)&31),false,false};
    if(v.signal!=0&&v.signal!=12&&v.signal!=14&&v.signal!=15&&v.signal!=30&&v.signal!=31)return v;
    if(v.signal==12){unsigned dst=unsigned((word>>46)&127);if(dst>=64)return v;
        v.writes=UINT64_C(1)<<dst;v.uniformFifo=true;
    }else if(word&FlagsMask)return v;
    if((word&AddMask)!=(Nop&AddMask)){
        unsigned op=unsigned((word>>24)&255),dst=unsigned((word>>32)&63),a=unsigned((word>>6)&63),b=unsigned(word&63);
        bool mov=op==249&&b==3&&v.signal==14,ia=v.signal==14,ib=v.signal==15;
        if((word&(UINT64_C(1)<<44))||dst>=64||(!Binary(op)&&!mov)||a>=(ia?48u:64u)||(!mov&&b>=(ib?48u:64u)))return v;
        if(v.writes&(UINT64_C(1)<<dst))return v;
        v.add=1;v.writes|=UINT64_C(1)<<dst;if(!ia)v.reads|=UINT64_C(1)<<a;if(!ib&&!mov)v.reads|=UINT64_C(1)<<b;
    }else if(v.signal==14||v.signal==15)return v;
    if((word&MulMask)!=(Nop&MulMask)){
        unsigned op=unsigned(word>>58),dst=unsigned((word>>38)&63),a=unsigned((word>>18)&63),b=unsigned((word>>12)&63);
        bool ia=v.signal==30,ib=v.signal==31;
        if(op!=21||(word&(UINT64_C(1)<<45))||dst>=64||a>=(ia?48u:64u)||b>=(ib?48u:64u)||(v.writes&(UINT64_C(1)<<dst)))return v;
        v.mul=1;v.writes|=UINT64_C(1)<<dst;if(!ia)v.reads|=UINT64_C(1)<<a;if(!ib)v.reads|=UINT64_C(1)<<b;
    }else if(v.signal==30||v.signal==31)return v;
    v.valid=v.add||v.mul||v.signal==12;return v;
}
inline bool Merge(Alu &first,const Alu &second){
    if(!first.valid||!second.valid||(first.add&&second.add)||(first.mul&&second.mul)||
       (first.signal&&second.signal)||(first.writes&(second.reads|second.writes)))return false;
    if(second.add)first.word=(first.word&~AddMask)|(second.word&AddMask);
    if(second.mul)first.word=(first.word&~MulMask)|(second.word&MulMask);
    if(second.signal)first.word=(first.word&~(SignalMask|FlagsMask))|(second.word&(SignalMask|FlagsMask));
    first.add|=second.add;first.mul|=second.mul;first.signal|=second.signal;
    first.reads|=second.reads;first.writes|=second.writes;first.uniformFifo|=second.uniformFifo;return true;
}
}
}
