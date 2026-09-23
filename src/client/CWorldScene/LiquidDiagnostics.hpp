// Internal read-only liquid context adapter. GPL-3.0-or-later.
#pragma once
#include "water/WaterDiagCore.hpp"
namespace wxl::waterdiag::native
{
struct Context {
    uint64_t invocation=0;unsigned depth=0;int pass=-1;uint32_t instances=0;
    Family family=Family::Unknown;Provider provider=Provider::Unknown;
    void *bank=nullptr,*transform=nullptr,*material=nullptr,*settings=nullptr,*geometry=nullptr,*animation=nullptr;
    const float *camera=nullptr,*world=nullptr,*sphere=nullptr;
    std::array<uint8_t,256> vsWrites{};std::array<uint8_t,224> psWrites{};
    unsigned vsWriteCalls=0,psWriteCalls=0,invalidWriteRanges=0;
};
using Boundary=void(*)(bool begin,const Context&) noexcept;
bool Install(Boundary callback); // Registers with existing hook chains; caller enables normal batch.
const Context* Current() noexcept;
void FloatWrite(bool pixel,unsigned first,unsigned count) noexcept;
bool Read(const void* address,void* output,size_t bytes) noexcept;
}
