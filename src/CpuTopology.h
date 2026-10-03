#pragma once
#include <windows.h>
#include <string>
#include <vector>

class CpuTopology {
public:
    enum class Mode { Auto, PhysicalCoresOnly, Smt25, Smt50, Smt75, AllLogicalProcessors };

    CpuTopology();
    int physicalCores() const { return physicalCores_; }
    int logicalProcessors() const { return logicalProcessors_; }
    bool smtAvailable() const { return logicalProcessors_ > physicalCores_; }
    int apply(Mode mode) const;
    std::wstring description() const;
    std::wstring name() const;

private:
    int physicalCores_ = 1;
    int logicalProcessors_ = 1;
    DWORD_PTR physicalMaskGroup0_ = 1;
    DWORD_PTR logicalMaskGroup0_ = 1;
    DWORD_PTR availableMaskGroup0_ = 1;
};
