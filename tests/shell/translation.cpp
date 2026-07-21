// Coverage for the shell's QEvent-to-interact translation, which the main
// doctest runner cannot reach: its target links no Qt by design — that is what
// keeps the interact layer headless — so the translation seam in sketchview.cpp
// had no test. REVIEW.md finding 12 asked for exactly one test through the shell
// translation once the engraved digit was derived from the scan code; this is
// it, widened to the pointer and modifier maps.
//
// A single QGuiApplication is constructed in main under the offscreen QPA
// plugin. QKeyEvent's constructor defaults its device to
// QInputDevice::primaryKeyboard(), which resolves through the application, and
// the runner has no display server. No SketchView is built — the helpers are
// pure, which is the whole point of exposing them.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointF>
#include <QString>
#include <Qt>

#include "core/geom.h"
#include "interact/events.h"
#include "interact/registry.h"
#include "shell/sketchview.h"

using namespace paroculus;

// The engraved digit is read from the native scan code first, then falls back
// to the Key_1..Key_9 keysyms. On X11 and Wayland the top-row digits occupy
// scan codes 10..18 whatever the layout or shift state.
TEST_CASE("engravedDigit reads the physical top-row key, not key() or text()") {
    // Shift+1..9 on a US layout: key() is the shifted symbol and text() is that
    // glyph, but scan codes 10..18 still carry the engraved digit 1..9.
    const int shiftedKeys[9] = {Qt::Key_Exclam,     Qt::Key_At,        Qt::Key_NumberSign,
                                Qt::Key_Dollar,     Qt::Key_Percent,   Qt::Key_AsciiCircum,
                                Qt::Key_Ampersand,  Qt::Key_Asterisk,  Qt::Key_ParenLeft};
    const char *shiftedText[9] = {"!", "@", "#", "$", "%", "^", "&", "*", "("};
    for(int i = 0; i < 9; i++) {
        QKeyEvent event(QEvent::KeyPress, shiftedKeys[i], Qt::ShiftModifier,
                        quint32(10 + i), /*nativeVirtualKey=*/0, /*nativeModifiers=*/0,
                        QString::fromLatin1(shiftedText[i]));
        CHECK(shelltest::engravedDigit(&event) == i + 1);
    }
}

TEST_CASE("engravedDigit falls back to the keysym outside the scan-code range") {
    SUBCASE("scan code 0, Key_5 -> 5") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier, 0, 0, 0,
                        QStringLiteral("5"));
        CHECK(shelltest::engravedDigit(&event) == 5);
    }
    SUBCASE("scan code 0, Key_Exclam -> 0, the documented dead shifted-digit case") {
        // No scan code and a symbol keysym: nothing says which digit was struck,
        // so the shifted-digit decline stays unreachable on this platform.
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier, 0, 0, 0,
                        QStringLiteral("!"));
        CHECK(shelltest::engravedDigit(&event) == 0);
    }
    SUBCASE("numpad 7: a scan code outside 10..18, Key_7 -> 7") {
        // evdev keypad-7 is 71, reported as 79 (offset by eight); the keysym
        // path answers because the scan code is out of the top-row range.
        QKeyEvent event(QEvent::KeyPress, Qt::Key_7, Qt::KeypadModifier, 79, 0, 0,
                        QStringLiteral("7"));
        CHECK(shelltest::engravedDigit(&event) == 7);
    }
}

TEST_CASE("strokeOf maps modifiers, extracts the character, and fills the digit") {
    SUBCASE("Shift sets only the Shift bit") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, 0, 0, 0,
                        QStringLiteral("A"));
        const KeyStroke stroke = shelltest::strokeOf(&event);
        CHECK(has(stroke.modifiers, Modifier::Shift));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Control));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Alt));
    }
    SUBCASE("Control sets only the Control bit") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier, 0, 0, 0,
                        QStringLiteral("a"));
        const KeyStroke stroke = shelltest::strokeOf(&event);
        CHECK(has(stroke.modifiers, Modifier::Control));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Shift));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Alt));
    }
    SUBCASE("Alt sets only the Alt bit") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::AltModifier, 0, 0, 0,
                        QStringLiteral("a"));
        const KeyStroke stroke = shelltest::strokeOf(&event);
        CHECK(has(stroke.modifiers, Modifier::Alt));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Shift));
        CHECK_FALSE(has(stroke.modifiers, Modifier::Control));
    }
    SUBCASE("every modifier at once sets every bit") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A,
                        Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier, 0, 0, 0,
                        QStringLiteral("A"));
        const KeyStroke stroke = shelltest::strokeOf(&event);
        CHECK(has(stroke.modifiers, Modifier::Shift));
        CHECK(has(stroke.modifiers, Modifier::Control));
        CHECK(has(stroke.modifiers, Modifier::Alt));
    }
    SUBCASE("character is the first char of text()") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0, 0, 0,
                        QStringLiteral("a"));
        CHECK(shelltest::strokeOf(&event).character == 'a');
    }
    SUBCASE("empty text leaves the character at 0") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Shift, Qt::NoModifier, 0, 0, 0, QString());
        CHECK(shelltest::strokeOf(&event).character == 0);
    }
    SUBCASE("digit is the engraved digit, carried apart from the character") {
        // Shift+3 on a US layout: text() is '#', scan code 12 is the engraved 3.
        QKeyEvent event(QEvent::KeyPress, Qt::Key_NumberSign, Qt::ShiftModifier, 12, 0, 0,
                        QStringLiteral("#"));
        const KeyStroke stroke = shelltest::strokeOf(&event);
        CHECK(stroke.digit == 3);
        CHECK(stroke.character == '#');
    }
}

TEST_CASE("translatePointer maps buttons, modifiers, and both coordinate spaces") {
    // Identity view: document coordinates equal screen pixels, so the position
    // can be asserted directly. The document<->screen mapping itself is
    // ViewTransform's and is tested in the interact layer.
    const ViewTransform identity;

    SUBCASE("left button and position") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(12.0, 34.0), Qt::LeftButton, Qt::NoModifier, identity,
            PointerAction::Press, 1);
        CHECK(e.button == Button::Left);
        CHECK(e.action == PointerAction::Press);
        CHECK(e.screen.x() == doctest::Approx(12.0));
        CHECK(e.screen.y() == doctest::Approx(34.0));
        CHECK(e.document.x == doctest::Approx(12.0));
        CHECK(e.document.y == doctest::Approx(34.0));
        CHECK(e.clicks == 1);
    }
    SUBCASE("middle button") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(0.0, 0.0), Qt::MiddleButton, Qt::NoModifier, identity,
            PointerAction::Press, 1);
        CHECK(e.button == Button::Middle);
    }
    SUBCASE("right button") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(0.0, 0.0), Qt::RightButton, Qt::NoModifier, identity,
            PointerAction::Press, 1);
        CHECK(e.button == Button::Right);
    }
    SUBCASE("no button on a move") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(5.0, 6.0), Qt::NoButton, Qt::NoModifier, identity,
            PointerAction::Move, 1);
        CHECK(e.button == Button::None);
        CHECK(e.action == PointerAction::Move);
    }
    SUBCASE("modifiers accumulate onto the pointer event") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(0.0, 0.0), Qt::LeftButton,
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier, identity,
            PointerAction::Move, 1);
        CHECK(has(e.modifiers, Modifier::Shift));
        CHECK(has(e.modifiers, Modifier::Control));
        CHECK(has(e.modifiers, Modifier::Alt));
    }
    SUBCASE("the click count passes through, so a double-click stays two") {
        const PointerEvent e = shelltest::translatePointer(
            QPointF(0.0, 0.0), Qt::LeftButton, Qt::NoModifier, identity,
            PointerAction::Press, 2);
        CHECK(e.clicks == 2);
    }
}

// DOCTEST_CONFIG_IMPLEMENT rather than the with-main variant, so the runner can
// stand a QGuiApplication up first. Only argv[0] is handed to Qt — the platform
// is forced through the environment — so doctest keeps the rest of the command
// line.
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int guiArgc = 1;
    QGuiApplication app(guiArgc, argv);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int failed = context.run();
    return failed;
}
