// Capture-current-value: the arithmetic that makes imposition movement-free.
//
// Adding a constraint to existing geometry declares intent about the present,
// and declaring intent about the present never rewrites the present. So an
// imposed distance becomes the distance that is already there, an imposed angle
// the angle already subtended, an imposed ratio the ratio already held. The
// geometry moves when a value is edited or something is dragged — not when the
// user says what was already true.
//
// This is the arithmetic half of that promise. It lives in core beside geom
// because both the action surface (what value to record) and the tests (whether
// anything moved) read it, and neither may see the other.
//
// The one exception PRINCIPLES names — near-parallel snapping shut — is not an
// exception here. Parallel carries no value, so there is nothing to capture;
// the motion is the solver's, and showing it is the surface's job.
#pragma once

#include <optional>
#include <span>

#include "core/pose.h"
#include "core/records.h"

namespace paroculus {

// The value `kind` currently holds over `operands`, in document units.
//
// operands: entity ids in the kind's operand order, at least as many as the
//   kind requires. kind: any constraint kind; a kind with no value returns
//   nullopt, which is the same answer a kind whose operands are missing gives.
//   Callers distinguish the two by asking the taxonomy first.
// Returns nullopt when the geometry cannot be resolved or the measurement is
//   undefined — a ratio against a zero-length segment, a distance from a point
//   to a degenerate segment.
//
// Angles come back in degrees, matching what the solver's angle constraint
// speaks and what a user types. Every other measurement is in document units.
std::optional<double> measure(const Pose &pose, ConstraintKind kind,
                              std::span<const EntityId> operands);

// The same question asked of a record that already exists, which is what a
// reference dimension's live readout is.
std::optional<double> measure(const Pose &pose, const ConstraintRecord &constraint);

// How far `constraint` is from being satisfied at this pose, in document units
// — or in degrees for the angular kinds, matching what measure() returns.
//
// Zero means it holds. This is what "movement-free" is asserted against: impose
// a constraint whose captured value is the measured one and the residual is
// already zero, so the solver has nothing to move. Returns nullopt when the
// geometry cannot be resolved.
//
// Deliberately geometric rather than a solver call. The property being tested
// is that the declaration agrees with the drawing, and asking the solver
// whether it converged tests something else — its own status code, which the
// selftest philosophy has never trusted.
std::optional<double> residual(const Pose &pose, const ConstraintRecord &constraint);

// Whether the two segments are near enough to parallel that imposing
// parallelism will visibly snap them shut.
//
// The one imposition that moves geometry, and the reason PRINCIPLES calls it
// the exception that proves the rule: a user who declares two nearly-parallel
// lines parallel is asking for the small angle to close, and the residual
// motion is shown rather than hidden. toleranceDegrees is a policy number the
// surface supplies; this only answers the geometric question.
//
// Returns nullopt when either segment cannot be resolved or is degenerate.
std::optional<double> parallelGapDegrees(const Pose &pose, EntityId a, EntityId b);

}  // namespace paroculus
