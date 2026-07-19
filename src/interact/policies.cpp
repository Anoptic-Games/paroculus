#include "interact/policies.h"

namespace paroculus {

int defaultHitPriority(const HitCandidate &candidate) {
    int priority = 0;
    if(candidate.kind == HitKind::Point) priority += 100;
    if(candidate.selected) priority += 20;
    if(candidate.role == Role::Construction) priority -= 50;
    return priority;
}

double snapScore(const SnapPolicy &policy, SnapTier tier, double correction,
                 std::optional<size_t> recentRank) {
    // Tier dominates by construction rather than by tuning: an auto-committing
    // coincidence must never lose to an offered parallel that happens to be
    // nearer, because the two are not competing for the same thing. One is
    // going to be declared; the other is going to be suggested.
    double score = 0.0;
    switch(tier) {
        case SnapTier::AutoCommit:    score += 2.0 * policy.tierWeight; break;
        case SnapTier::Offered:       score += 1.0 * policy.tierWeight; break;
        case SnapTier::PlacementOnly: break;
    }
    // Nearer wins within a tier. Negative so a larger correction scores lower.
    score -= correction * policy.closenessWeight;
    // Document-local recency: the kind used most recently gets the full bonus,
    // older ones progressively less. Deterministic and inspectable — this is
    // the whole of "contextual", and there is deliberately nothing learned.
    if(recentRank) {
        score += policy.recencyBonus /
                 static_cast<double>(*recentRank + 1);
    }
    return score;
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
