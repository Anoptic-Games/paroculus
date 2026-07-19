// Geometry primitives and the document<->screen transform type. Core is the
// bottom of the stack: Eigen and the standard library, nothing else.
#pragma once

#include <Eigen/Geometry>

#include <algorithm>

namespace paroculus {

// A point in document space. Document units are nominally millimetres, Y-up.
struct Point {
    double x = 0.0;
    double y = 0.0;
};

// The one document<->screen mapping. It lives in core because hit testing,
// snapping and rendering must all convert through the same implementation —
// every zoom bug is a leak between the pixel regime and the document regime,
// so the conversion happens here and only here. View *state* (what the
// transform currently is, and who changes it) belongs to the shell.
//
// Screen space is Y-down pixels; document space is Y-up document units, so a
// well-formed view carries a negative Y scale.
// Invariant: the affine is invertible. The inverse is computed once at
// construction because pixel tolerances are inverse-transformed per query.
class ViewTransform {
public:
    ViewTransform() = default;

    // m: document -> screen. Must be invertible.
    explicit ViewTransform(const Eigen::Affine2d &m) : m_(m), inv_(m.inverse()) {}

    // p: document coordinates. Returns screen pixels.
    Eigen::Vector2d toScreen(const Point &p) const { return m_ * Eigen::Vector2d(p.x, p.y); }

    // v: screen pixels. Returns document coordinates.
    Point toDocument(const Eigen::Vector2d &v) const {
        const Eigen::Vector2d d = inv_ * v;
        return {d.x(), d.y()};
    }

    // r: a radius in pixels. Returns the document-space radius it covers,
    // taking the larger axis so tolerances never shrink under anisotropic zoom.
    double toDocumentLength(double r) const {
        const Eigen::Matrix2d l = inv_.linear();
        return r * std::max(l.col(0).norm(), l.col(1).norm());
    }

    const Eigen::Affine2d &matrix() const { return m_; }

private:
    Eigen::Affine2d m_ = Eigen::Affine2d::Identity();
    Eigen::Affine2d inv_ = Eigen::Affine2d::Identity();
};

}  // namespace paroculus
