#pragma once
#include <cstdint>
namespace wxl::water::slot3 {
// Logging mode/budget/sink do not participate in production availability.
inline bool SnapshotAttemptPermitted(bool productionQualified,bool diagnosticCopy,
    std::uint64_t processAttempts,std::uint64_t generationAttempts) noexcept {
    return productionQualified || (diagnosticCopy && processAttempts<8 && generationAttempts<4);
}
inline bool ResourceHooksNeeded(bool productionRequested,bool diagnosticsRequested) noexcept {
    return productionRequested || diagnosticsRequested;
}
}
