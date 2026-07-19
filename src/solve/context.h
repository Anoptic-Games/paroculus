// Solve contexts as cheap value types.
//
// One capability, exercised five ways: warm-started drags, speculative
// previews, creation-time validation, asynchronous solving and later animation
// evaluation are all "fork a component's parameters, solve the copy, throw it
// away". Copying a context has to be cheap enough to do on hover, which is why
// it holds parameter spans for one component and not a document.
//
// The document is immutable throughout. Solved values land in the context, and
// writing them back as seeds is a separate, explicit command — so a preview
// cannot mutate anything by forgetting to.
#pragma once

#include <optional>
#include <vector>

#include "core/document.h"
#include "core/geom.h"
#include "core/topology.h"

namespace paroculus {

class SolveContext {
public:
    SolveContext() = default;

    // The component containing `anchor`, seeded from the document's seeds.
    // Returns an empty context when the anchor is not live.
    static SolveContext forComponent(const Document &doc, const Topology &topology,
                                     EntityId anchor);

    // Every entity in the document as one context. The demo's shape, and the
    // fallback when no partition is wanted; real interaction scopes to a
    // component, which is what keeps a drag local.
    // The union of the components containing each anchor. A candidate
    // constraint spanning two components has to be tested against both, since
    // committing it would merge them.
    static SolveContext forComponents(const Document &doc, const Topology &topology,
                                      const std::vector<EntityId> &anchors);

    static SolveContext forWholeDocument(const Document &doc);

    // An explicit member list, for a caller that has already partitioned.
    // Solving every component in turn would otherwise ask the topology for each
    // one's members separately, and each of those asks is a scan of the whole
    // document — quadratic in a document that is mostly loose geometry, which
    // is the shape the per-component solve exists to be cheap on.
    static SolveContext forMembers(const Document &doc, std::vector<EntityId> members);

    // ID-ordered. Determines translation order, and therefore determinism.
    const std::vector<EntityId> &members() const { return members_; }

    // Parameter values, ID-ordered over the param-owning members. Seeds going
    // in, solved values coming out — the same storage, which is what makes a
    // warm start the default rather than an optimisation.
    const std::vector<SeedSpan> &params() const { return params_; }
    std::vector<SeedSpan> &params() { return params_; }

    bool contains(EntityId id) const;
    bool empty() const { return members_.empty(); }

    // Solved (or seeded) values by document identity, never by array position.
    std::optional<Point> point(EntityId id) const;
    std::optional<double> radius(EntityId id) const;

    // The commands that write these values back to the document as seeds.
    //
    // This is release-commits-seeds: the solved state at mouse-up becomes the
    // new seeds, nothing springs back, and because it is an ordinary command
    // list it rides the undo journal like every other edit. Records whose
    // values are unchanged are omitted, so a no-op drag journals nothing.
    std::vector<Command> commitCommands(const Document &doc) const;

    friend bool operator==(const SolveContext &a, const SolveContext &b) {
        return a.members_ == b.members_ && a.params_ == b.params_;
    }
    friend bool operator!=(const SolveContext &a, const SolveContext &b) { return !(a == b); }

private:
    static SolveContext build(const Document &doc, std::vector<EntityId> members);

    std::vector<EntityId> members_;
    std::vector<SeedSpan> params_;
};

}  // namespace paroculus
