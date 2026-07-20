# Turns a TTF into a C++ translation unit holding its bytes.
#
# Embedded rather than referenced, because the typeface is part of what the
# renderer *is*: dimension text has to look the same in the sandbox, in a golden
# raster comparison and on a user's machine, and a font resolved at runtime
# makes all three answer differently. A store path baked into the binary would
# be nix-specific and would break the moment the app is copied anywhere; the
# bytes cannot.
#
# Invoked at configure time with -DFONT= and -DOUT=.
file(READ "${FONT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR count "${length} / 2")

# One regex pass rather than a loop over hundreds of thousands of offsets:
# CMake's string handling is slow enough that the loop form takes minutes on a
# full-coverage face, and a configure step nobody wants to wait for is a
# configure step somebody will work around.
string(REGEX REPLACE "(..)" "0x\\1," body "${hex}")

file(WRITE "${OUT}" "\
// Generated from ${FONT} at configure time. Do not edit.
#include \"render/typeface.h\"

namespace paroculus {
namespace {
// Aligned, because Skia may read the table directory as wider words than bytes.
alignas(8) const unsigned char BYTES[] = {
${body}
};
}  // namespace

std::span<const unsigned char> bundledTypefaceBytes() {
    return std::span<const unsigned char>(BYTES, ${count});
}

}  // namespace paroculus
")
