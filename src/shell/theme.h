// The theme tokens, dark by default.
//
// The frame's inline hex constants promoted to one place, so a re-skin is a
// value change here rather than a sweep across the QML — the payoff the spec
// asks for while holding the theme itself loosely. A QML singleton, so any QML
// reads `Theme.panelBg` after importing the module.
//
// Deliberately not the render palette: the canvas tints (selection, resistance,
// roles) are semantics, not theme, and stay render's own. What lives here is
// chrome — window, docks, panels, text, borders, and the two diagnostic accents.
//
// Header-only: the class is small and every token is a CONSTANT member, so there
// is nothing to define out of line; theme.cpp exists only to give AUTOMOC a
// translation unit.
#pragma once

#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace paroculus {

class Theme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Backgrounds, darkest to lightest.
    Q_PROPERTY(QColor windowBg MEMBER windowBg_ CONSTANT)
    Q_PROPERTY(QColor dockBg MEMBER dockBg_ CONSTANT)
    Q_PROPERTY(QColor panelBg MEMBER panelBg_ CONSTANT)
    Q_PROPERTY(QColor headerBg MEMBER headerBg_ CONSTANT)
    Q_PROPERTY(QColor surface MEMBER surface_ CONSTANT)
    Q_PROPERTY(QColor surfaceRaised MEMBER surfaceRaised_ CONSTANT)
    Q_PROPERTY(QColor fieldBg MEMBER fieldBg_ CONSTANT)
    // Interaction states.
    Q_PROPERTY(QColor hover MEMBER hover_ CONSTANT)
    Q_PROPERTY(QColor hoverStrong MEMBER hoverStrong_ CONSTANT)
    Q_PROPERTY(QColor activeBg MEMBER activeBg_ CONSTANT)
    // Borders.
    Q_PROPERTY(QColor border MEMBER border_ CONSTANT)
    Q_PROPERTY(QColor borderStrong MEMBER borderStrong_ CONSTANT)
    // Text, brightest to faintest.
    Q_PROPERTY(QColor textBright MEMBER textBright_ CONSTANT)
    Q_PROPERTY(QColor textPrimary MEMBER textPrimary_ CONSTANT)
    Q_PROPERTY(QColor textSecondary MEMBER textSecondary_ CONSTANT)
    Q_PROPERTY(QColor textMuted MEMBER textMuted_ CONSTANT)
    Q_PROPERTY(QColor textDim MEMBER textDim_ CONSTANT)
    Q_PROPERTY(QColor textFaint MEMBER textFaint_ CONSTANT)
    // Diagnostic accents: calm info, and the two warm resistance/warning tones.
    Q_PROPERTY(QColor info MEMBER info_ CONSTANT)
    Q_PROPERTY(QColor warn MEMBER warn_ CONSTANT)
    Q_PROPERTY(QColor warnAlt MEMBER warnAlt_ CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr) : QObject(parent) {}

private:
    QColor windowBg_ = QColor(QStringLiteral("#0e1013"));
    QColor dockBg_ = QColor(QStringLiteral("#15181d"));
    QColor panelBg_ = QColor(QStringLiteral("#191c21"));
    QColor headerBg_ = QColor(QStringLiteral("#20242b"));
    QColor surface_ = QColor(QStringLiteral("#22262d"));
    QColor surfaceRaised_ = QColor(QStringLiteral("#252b34"));
    QColor fieldBg_ = QColor(QStringLiteral("#141820"));
    QColor hover_ = QColor(QStringLiteral("#1d2229"));
    QColor hoverStrong_ = QColor(QStringLiteral("#2a3038"));
    QColor activeBg_ = QColor(QStringLiteral("#3a4557"));
    QColor border_ = QColor(QStringLiteral("#333944"));
    QColor borderStrong_ = QColor(QStringLiteral("#3d4553"));
    QColor textBright_ = QColor(QStringLiteral("#e6e9ee"));
    QColor textPrimary_ = QColor(QStringLiteral("#c8d2e0"));
    QColor textSecondary_ = QColor(QStringLiteral("#aab1bd"));
    QColor textMuted_ = QColor(QStringLiteral("#7f8794"));
    QColor textDim_ = QColor(QStringLiteral("#5c646f"));
    QColor textFaint_ = QColor(QStringLiteral("#454b55"));
    QColor info_ = QColor(QStringLiteral("#7cc4e0"));
    QColor warn_ = QColor(QStringLiteral("#e0c07c"));
    QColor warnAlt_ = QColor(QStringLiteral("#e0a37c"));
};

}  // namespace paroculus
