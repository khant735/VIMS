#include "CpuTopology.h"
#include <bit>
#include <sstream>

static DWORD_PTR lowestBit(DWORD_PTR v) {
    return v & (~v + 1);
}

CpuTopology::CpuTopology() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<unsigned char> data(len);
    if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()), &len)) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        logicalProcessors_ = static_cast<int>(si.dwNumberOfProcessors);
        physicalCores_ = logicalProcessors_;
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            physicalMaskGroup0_ = logicalMaskGroup0_ = processMask;
        }
        availableMaskGroup0_=logicalMaskGroup0_;
        return;
    }

    physicalCores_ = 0;
    logicalProcessors_ = 0;
    physicalMaskGroup0_ = 0;
    logicalMaskGroup0_ = 0;

    DWORD offset = 0;
    while (offset < len) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data() + offset);
        if (info->Relationship == RelationProcessorCore) {
            ++physicalCores_;
            const auto& proc = info->Processor;
            for (WORD i = 0; i < proc.GroupCount; ++i) {
                const auto& ga = proc.GroupMask[i];
                logicalProcessors_ += static_cast<int>(std::popcount(static_cast<unsigned long long>(ga.Mask)));
                if (ga.Group == 0) {
                    logicalMaskGroup0_ |= ga.Mask;
                    physicalMaskGroup0_ |= lowestBit(ga.Mask);
                }
            }
        }
        offset += info->Size;
    }

    if (physicalCores_ < 1) physicalCores_ = 1;
    if (logicalProcessors_ < 1) logicalProcessors_ = physicalCores_;
    if (!physicalMaskGroup0_) physicalMaskGroup0_ = 1;
    if (!logicalMaskGroup0_) logicalMaskGroup0_ = physicalMaskGroup0_;
    DWORD_PTR processMask=0,systemMask=0;
    if(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask))availableMaskGroup0_=processMask;
    else availableMaskGroup0_=logicalMaskGroup0_;
}

int CpuTopology::apply(Mode mode) const {
    // Retain the original process mask so selecting Auto restores all available CPUs.
    const DWORD_PTR allowed=availableMaskGroup0_;
    const DWORD_PTR physical=physicalMaskGroup0_ & allowed;
    const DWORD_PTR logical=logicalMaskGroup0_ & allowed;
    DWORD_PTR mask=allowed;
    if(mode==Mode::PhysicalCoresOnly)mask=physical;
    else if(mode==Mode::AllLogicalProcessors)mask=logical;
    else if(mode==Mode::Smt25||mode==Mode::Smt50||mode==Mode::Smt75){
        const int base=static_cast<int>(std::popcount(static_cast<unsigned long long>(physical)));
        const int extra=static_cast<int>(std::popcount(static_cast<unsigned long long>(logical & ~physical)));
        const int percent=mode==Mode::Smt25?25:mode==Mode::Smt50?50:75;
        const int add=(extra*percent+50)/100;
        mask=physical;
        DWORD_PTR siblings=logical & ~physical;
        for(int i=0;i<add&&siblings;++i){DWORD_PTR bit=lowestBit(siblings);mask|=bit;siblings&=~bit;}
    }
    if(!mask)mask=allowed?allowed:DWORD_PTR(1);
    if(!SetProcessAffinityMask(GetCurrentProcess(),mask)){
        DWORD_PTR current=0,system=0;
        if(GetProcessAffinityMask(GetCurrentProcess(),&current,&system))mask=current;
    }
    const int threads=static_cast<int>(std::popcount(static_cast<unsigned long long>(mask)));
    return threads>0?threads:1;
}

std::wstring CpuTopology::description() const {
    std::wstringstream ss;
    ss << physicalCores_ << L" physical core" << (physicalCores_ == 1 ? L"" : L"s")
       << L" / " << logicalProcessors_ << L" logical processor" << (logicalProcessors_ == 1 ? L"" : L"s");
    if (smtAvailable()) ss << L" (SMT/Hyper-Threading detected)";
    return ss.str();
}
