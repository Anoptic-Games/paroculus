// Transforms against constraints.
//
// Vector users expect free transform of anything; a parametric document pushes
// back in two specific places, and both are answered by modelling rather than by
// a warning dialog. A transform here is a seed rewrite and nothing else: every
// point the selection reaches moves exactly, the document's relations are
// untouched unless the user answers a question that says otherwise, and the
// solve that follows is an ordinary one.
//
// That is what makes the isometry property testable. A rigidly constrained
// cluster rotated about any centre has every internal residual still at zero
// before anything re-solves, because every relation in the catalogue that is not
// axis-referenced is invariant under rigid motion — so the re-solve is the
// identity, not a settling.
//
// The two frictions and their answers:
//
// Rotation versus axis constraints. Horizontal and vertical are not intrinsic
// properties, they are parallelism to a reference axis with the document frame
// as the default, and the format has carried the nullable reference since stage
// 4. So rotating a subset that carries them is a real question with two honest
// answers — retarget them to a frame that tilts with the cluster, or keep the
// document axes and let the solver fight the rotation — and the answer is a
// parameter on the transform rather than a heuristic inside it.
//
// Scale versus absolute dimensions. The taxonomy already says which relations
// survive a uniform scale: `Invariance`. The scale-invariant family needs
// nothing, the absolute family is the question, and the answer is again a
// parameter — rewrite the slots by the factor, or leave them to resist.
//
// Non-uniform scale is refused here rather than approximated. It does not
// commute with almost any relation in the catalogue, so a document that survived
// one would be a document whose constraints no longer said what they said. It
// remains available at the export bake, which is honest about being lossy.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "core/document.h"
#include "core/geom.h"

namespace paroculus {

// Why a transform produced no commands. Refusal is total: a refused transform
// leaves the document byte-identical, exactly as a refused command does.
enum class TransformError : uint8_t {
    None,
    NothingToMove,  // the selection reaches no parameter a transform could write
    Degenerate,     // a zero scale factor, which is not a transform but a collapse
    NonUniform,     // refused in-model; available only at the export bake
    Locked,         // some of what would move is locked, and a lock does not move
};

const char *transformErrorName(TransformError e);

// Every entity a transform would write parameters into: the selection closed
// over the points that define it.
//
// Closed downward and never upward. Selecting a segment moves its endpoints,
// because a segment owns nothing and is entirely its endpoints; selecting one
// endpoint does not move the segment's other end, because the user named one
// point and moving the rest would be moving what they did not select. A circle
// contributes its centre and its own radius parameter.
//
// In ID order, so a transform over the same selection produces the same command
// sequence every time — determinism is a document property and a transform is a
// document edit.
std::vector<EntityId> transformClosure(const Document &doc,
                                       std::span<const EntityId> selection);

// The axis-referenced relations a rotation makes a question of: horizontal and
// vertical whose required segment lies inside the moved set and whose reference
// is still the document frame.
//
// One that already names a reference axis is not a question — the user has
// already said which frame it belongs to, and a rotation that moves both the
// segment and its axis carries the relation along untouched. That is the whole
// payoff of recording axis constraints as parallelism rather than as a property.
//
// In ID order.
std::vector<ConstraintId> axisReferencedIn(const Document &doc,
                                           std::span<const EntityId> moved);

// The absolute-valued relations wholly inside the moved set: what
// scale-the-values rewrites and let-them-resist leaves alone.
//
// Wholly inside, because a distance from a moved point to a stationary one is
// not a dimension of the thing being scaled — it is a relation between the
// scaled thing and its surroundings, and multiplying it by the factor would be
// asserting something about geometry the user did not select. Those resist
// whichever answer is given, which is correct and is what the drop count on the
// result reports.
//
// In ID order.
std::vector<ConstraintId> absoluteValuedIn(const Document &doc,
                                           std::span<const EntityId> moved);

// A transform, as commands plus what the surface has to say about it.
//
// `commands` is empty exactly when `error` is set. The counts are the
// no-silent-changes half: a transform that retargeted four axis constraints or
// rewrote three dimensions has done something to the document beyond moving it,
// and the user is told rather than left to find out.
struct TransformStep {
    std::vector<Command> commands;
    TransformError error = TransformError::None;

    // Entities whose seeds this rewrites.
    size_t moved = 0;
    // Axis-referenced relations bound to the new cluster frame.
    size_t retargeted = 0;
    // Absolute-valued relations whose slots were multiplied by the factor.
    size_t rescaled = 0;
    // Absolute-valued relations that reach outside the moved set, so they were
    // left alone whichever answer was given, and will resist.
    size_t straddling = 0;
    // The construction segment a retarget created, or null. Named so a surface
    // can show the user the frame their cluster now belongs to.
    EntityId frame;

    bool ok() const { return error == TransformError::None; }
};

// How a rotation answers the axis question.
enum class AxisAnswer : uint8_t {
    // Keep the document axes. The rotated edges are still declared horizontal
    // against the document frame, so the solver pulls them back and the cluster
    // resists the rotation. Honest, and sometimes what is wanted.
    KeepDocumentAxes,
    // Retarget to a frame that tilts with the cluster. One construction segment
    // is created at the rotated angle and every axis-referenced relation in the
    // moved set names it, so the subset's "horizontal" is now its own.
    RetargetToClusterFrame,
};

struct RotateOptions {
    Point centre;
    double angle = 0.0;  // radians, counter-clockwise, matching the solver
    AxisAnswer axes = AxisAnswer::KeepDocumentAxes;
};

// How a uniform scale answers the absolute-dimension question.
enum class ValueAnswer : uint8_t {
    // Leave the slots alone. The geometry moves and the dimensions pull it
    // back, so a dimensioned drawing resists being scaled — which is what an
    // absolute dimension is for.
    LetThemResist,
    // Multiply each absolute-valued slot inside the moved set by the factor, so
    // the drawing rescales and keeps holding what it held. Trivially expressible
    // because values are slots: the rewrite is a factor node over the existing
    // expression, so a dimension driven by a named parameter stays driven by it.
    ScaleTheValues,
};

struct ScaleOptions {
    Point centre;
    double factor = 1.0;
    ValueAnswer values = ValueAnswer::LetThemResist;
};

// Rotates the selection about `centre` by `angle`.
//
// The rewrite is exact: a point's new seeds are the rotation of its old ones,
// computed once at full precision, and nothing is routed through a solve to get
// there. Circle radii and every relation's value are untouched, because a
// rotation changes neither.
TransformStep rotateStep(const Document &doc, std::span<const EntityId> selection,
                         const RotateOptions &options);

// Scales the selection about `centre` by `factor`.
//
// Uniform only. A caller wanting different factors per axis is asking for
// something the model refuses, and refusing needs the two factors to be
// comparable, which is why non-uniformity is a distinct error rather than an
// absent overload.
TransformStep scaleStep(const Document &doc, std::span<const EntityId> selection,
                        const ScaleOptions &options);

// Refuses, always, and says why. The whole of non-uniform scale in the model:
// present so the action surface can offer it and report the honest answer,
// rather than the action being missing and the user concluding the tool forgot.
TransformStep nonUniformScaleStep(const Document &doc, std::span<const EntityId> selection,
                                  double factorX, double factorY);

// Translates the selection by (dx, dy).
//
// Not a question anywhere: every relation in the catalogue is
// translation-invariant, axis-referenced ones included, so this never retargets
// and never rescales. Here rather than in interact because duplicate-with-offset
// needs it and a drag is a different mechanism entirely — a drag is a solve, and
// this is a rewrite.
TransformStep translateStep(const Document &doc, std::span<const EntityId> selection, double dx,
                            double dy);

}  // namespace paroculus
