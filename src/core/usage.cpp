#include "core/usage.h"

#include <limits>

namespace paroculus {

void UsageHistory::note(ConstraintKind kind) {
    uint32_t &c = counts_[static_cast<size_t>(kind)];
    if(c < std::numeric_limits<uint32_t>::max()) c++;
}

std::vector<std::pair<ConstraintKind, uint32_t>> UsageHistory::entries() const {
    std::vector<std::pair<ConstraintKind, uint32_t>> out;
    for(size_t i = 0; i < counts_.size(); i++) {
        if(counts_[i] != 0) out.emplace_back(static_cast<ConstraintKind>(i), counts_[i]);
    }
    return out;
}

bool UsageHistory::empty() const {
    for(uint32_t c : counts_) {
        if(c != 0) return false;
    }
    return true;
}

}  // namespace paroculus
