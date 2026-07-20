// The bundled typeface.
//
// Dimension text is the one adorner that carries a value rather than a shape,
// and a value has to be legible identically everywhere: in the sandbox, in a
// raster comparison, and on a machine whose fontconfig has never been asked
// about anything. So the face is compiled in rather than resolved at runtime —
// a golden that depended on what the host happened to have installed would be a
// golden about the host.
//
// The bytes come from a pinned build input and are turned into a translation
// unit at configure time by cmake/embed_font.cmake. Nothing above render knows
// the font exists: what a mark looks like is render's business, and which marks
// exist is not.
#pragma once

#include <span>

namespace paroculus {

// The face's file bytes. Non-empty in every configuration, because a build
// without a typeface is refused at configure time rather than producing an
// application whose dimensions are silently blank.
std::span<const unsigned char> bundledTypefaceBytes();

}  // namespace paroculus
