#include "interact/policies.h"

namespace paroculus {

int defaultHitPriority(const HitCandidate &candidate) {
    int priority = 0;
    if(candidate.kind == HitKind::Point) priority += 100;
    if(candidate.selected) priority += 20;
    if(candidate.role == Role::Construction) priority -= 50;
    return priority;
}

bool hitBeats(const HitCandidate &a, const HitCandidate &b) {
    const int pa = defaultHitPriority(a);
    const int pb = defaultHitPriority(b);
    if(pa != pb) return pa > pb;
    if(a.distance != b.distance) return a.distance < b.distance;
    // Ties break on ID so hover never flickers between two coincident things.
    return a.entity < b.entity;
}

}  // namespace paroculus
