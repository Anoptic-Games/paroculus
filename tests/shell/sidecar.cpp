// The per-document view sidecar, in isolation. The determinism property that
// pairs with it — open with and without the sidecar, edit identically, save
// byte-identically — is a workspace-level test and lives beside the workspace,
// because it is the workspace that keeps document and sidecar apart. Here the
// sidecar is proven to round-trip exactly and to be droppable field by field.
//
// No main and no DOCTEST_CONFIG_IMPLEMENT: translation.cpp owns both for this
// runner, and stands a QGuiApplication up first. These cases need none of it.
#include <doctest/doctest.h>

#include "shell/sidecar.h"

using namespace paroculus;

TEST_CASE("the sidecar path is the document's own name with -view on its extension") {
    CHECK(sidecarPathFor("/foo/drawing.paro") == "/foo/drawing.paro-view");
    CHECK(sidecarPathFor("drawing.paro") == "drawing.paro-view");
    // A path with dots before the extension keeps them.
    CHECK(sidecarPathFor("/a/my.drawing.paro") == "/a/my.drawing.paro-view");
    // No .paro extension: appended whole, so an imported-SVG workspace or an
    // as-yet-unsaved one still has a defined, collision-free sidecar name.
    CHECK(sidecarPathFor("/foo/traced") == "/foo/traced.paro-view");
    CHECK(sidecarPathFor("/foo/image.svg") == "/foo/image.svg.paro-view");
}

TEST_CASE("the sidecar round-trips pan and zoom exactly") {
    Sidecar in;
    in.panX = -123.456789012345;
    in.panY = 987654.3210987654;
    in.zoom = 0.05;  // the zoom clamp floor

    const Sidecar out = readSidecar(writeSidecar(in));
    // Exact, not approximate: the view framing is a double and the format is
    // lossless, the same property the document format holds for its seeds.
    CHECK(out.panX == in.panX);
    CHECK(out.panY == in.panY);
    CHECK(out.zoom == in.zoom);
}

TEST_CASE("the sidecar round-trips the U2 presentation fields") {
    Sidecar in;
    in.panX = 12.5;
    in.background = 0xff2a3d55u;
    in.showAllFrames = true;
    in.extensions = true;
    in.gridVisible = false;
    const Sidecar out = readSidecar(writeSidecar(in));
    CHECK(out.panX == 12.5);
    CHECK(out.background == 0xff2a3d55u);
    CHECK(out.showAllFrames);
    CHECK(out.extensions);
    CHECK_FALSE(out.gridVisible);
}

TEST_CASE("a version-0 sidecar reader survives the U2 fields, keeping their defaults") {
    // The additive-fields discipline: a reader that predates the fields skips
    // them, so a document round-trips through an old build with its view intact
    // and only the newer preferences shed.
    const Sidecar out = readSidecar("paro-view 0\nview 3.0 4.0 1.5\n");
    CHECK(out.panX == 3.0);
    CHECK(out.zoom == 1.5);
    // The defaults, since the lines are absent.
    CHECK(out.background == 0u);
    CHECK_FALSE(out.showAllFrames);
    CHECK_FALSE(out.extensions);
    CHECK(out.gridVisible);
}

TEST_CASE("a malformed presentation field keeps its default") {
    const Sidecar out =
        readSidecar("paro-view 0\nbackground not-a-number\ngrid also-bad\n");
    CHECK(out.background == 0u);
    CHECK(out.gridVisible);  // unchanged from its true default
}

TEST_CASE("a malformed sidecar keeps defaults rather than coercing to zero") {
    // Droppable field by field: a preference that will not parse costs that
    // preference and no more. A coerced zero zoom would be a collapsed view, a
    // silent change the loader's parse-or-refuse discipline exists to prevent —
    // but here the answer is keep-the-default, because a view preference is not
    // worth refusing a whole open over.
    const Sidecar out = readSidecar("paro-view 0\nview not-a-number also-bad 2.0\n");
    CHECK(out.panX == 0.0);
    CHECK(out.panY == 0.0);
    // The one well-formed field on the line is still taken.
    CHECK(out.zoom == 2.0);
}

TEST_CASE("an empty or foreign sidecar text yields defaults") {
    CHECK(readSidecar("").zoom == 1.0);
    CHECK(readSidecar("garbage\nlines\nhere\n").zoom == 1.0);
    CHECK(readSidecar("garbage").panX == 0.0);
}
