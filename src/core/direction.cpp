#include "core/direction.h"

namespace paroculus {
namespace {

// A flat union-find over a dense index space. The segments occupy 0..n-1; the
// sentinels (the document horizontal, the document vertical, and one per named
// reference's perpendicular) are appended as they are needed, so a document that
// names none of them pays for none.
struct UnionFind {
    std::vector<int> parent;

    int make() {
        parent.push_back(static_cast<int>(parent.size()));
        return static_cast<int>(parent.size()) - 1;
    }
    int find(int i) {
        while(parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

}  // namespace

std::vector<EntityId> DirectionClasses::classMembers(EntityId segment) const {
    const auto it = classOf.find(segment);
    if(it == classOf.end()) return {};
    return members[static_cast<size_t>(it->second)];
}

DirectionClasses directionClasses(const Document &doc) {
    UnionFind uf;
    // Segments in id order — records() is id-ordered — so the dense indices, and
    // therefore the class order derived from them, are deterministic.
    std::unordered_map<EntityId, int> segmentIndex;
    std::vector<EntityId> segments;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind != EntityKind::Segment) continue;
        segmentIndex[e.id] = uf.make();
        segments.push_back(e.id);
    }

    // The two document-frame sentinels and the per-reference perpendicular ones,
    // all created lazily so a document that declares no such relation grows no
    // extra class.
    int documentHorizontal = -1;
    int documentVertical = -1;
    std::unordered_map<EntityId, int> perpendicularOf;
    auto ensure = [&](int &slot) {
        if(slot < 0) slot = uf.make();
        return slot;
    };
    auto perpendicular = [&](EntityId reference) {
        const auto it = perpendicularOf.find(reference);
        if(it != perpendicularOf.end()) return it->second;
        const int made = uf.make();
        perpendicularOf[reference] = made;
        return made;
    };
    auto indexOf = [&](EntityId id) -> int {
        const auto it = segmentIndex.find(id);
        return it != segmentIndex.end() ? it->second : -1;
    };

    for(const ConstraintRecord &c : doc.constraints().records()) {
        const size_t bound = boundOperandCount(c);
        switch(c.kind) {
            case ConstraintKind::Parallel: {
                const int a = indexOf(c.operands[0]);
                const int b = indexOf(c.operands[1]);
                if(a >= 0 && b >= 0) uf.unite(a, b);
                break;
            }
            case ConstraintKind::Horizontal: {
                const int seg = indexOf(c.operands[0]);
                if(seg < 0) break;
                const EntityId reference = bound > 1 ? c.operands[1] : EntityId();
                const int ref = reference.valid() ? indexOf(reference) : -1;
                // Named reference: the segment is parallel to it. No reference:
                // the document horizontal.
                if(ref >= 0) uf.unite(seg, ref);
                else uf.unite(seg, ensure(documentHorizontal));
                break;
            }
            case ConstraintKind::Vertical: {
                const int seg = indexOf(c.operands[0]);
                if(seg < 0) break;
                const EntityId reference = bound > 1 ? c.operands[1] : EntityId();
                // Named reference: the segment is perpendicular to it, so it
                // joins the reference's perpendicular class — shared by every
                // vertical about that reference, and not the reference's own.
                if(reference.valid() && indexOf(reference) >= 0) {
                    uf.unite(seg, perpendicular(reference));
                } else {
                    uf.unite(seg, ensure(documentVertical));
                }
                break;
            }
            default:
                break;
        }
    }

    // One class per distinct root among the segments, ordered by the smallest
    // member — which is the first-seen member, since segments are walked in id
    // order. Sentinels never enter `members`: they only join segments.
    DirectionClasses out;
    std::unordered_map<int, int> classIndexOfRoot;
    for(EntityId id : segments) {
        const int root = uf.find(segmentIndex[id]);
        const auto it = classIndexOfRoot.find(root);
        int classIndex;
        if(it == classIndexOfRoot.end()) {
            classIndex = static_cast<int>(out.members.size());
            classIndexOfRoot[root] = classIndex;
            out.members.emplace_back();
        } else {
            classIndex = it->second;
        }
        out.members[static_cast<size_t>(classIndex)].push_back(id);
        out.classOf[id] = classIndex;
    }
    return out;
}

}  // namespace paroculus
