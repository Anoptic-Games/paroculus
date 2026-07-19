#include "shell/sketchview.h"

#include <QPainter>

#include "render/view.h"
#include "solve/demosketch.h"

SketchView::SketchView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    resolve();
}

void SketchView::setRatio(qreal ratio) {
    if(qFuzzyCompare(ratio_, ratio) || ratio <= 0.0) return;
    ratio_ = ratio;
    resolve();
    update();
    emit solutionChanged();
}

// Stage-0 residue: the shell calls solve directly because the interact layer
// that will own this dispatch does not exist until stage 3.
void SketchView::resolve() {
    solution_ = paroculus::solveDemoSketch(ratio_);
}

// Degrees of freedom read calmly, never as a progress bar or a warning: under-
// constraint is the normal state, and a free DOF is exactly a thing the user
// can still push by hand. Solve time sits beside it because the interactive
// budget is a measured thing and this is the term that varies.
QString SketchView::status() const {
    return QStringLiteral("solver: %1  ·  dof: %2  ·  solve: %3 ms  ·  ratio: %4")
        .arg(QString::fromLatin1(paroculus::statusName(solution_.status)))
        .arg(solution_.dof)
        .arg(solution_.microseconds / 1000.0, 0, 'f', 2)
        .arg(ratio_, 0, 'f', 3);
}

// Skia renders into the QImage's own pixels, so the only copy is Qt's final
// blit. Format_ARGB32_Premultiplied is byte-for-byte kBGRA_8888/kPremul on
// little-endian, which is what renderSketch writes.
void SketchView::paint(QPainter *painter) {
    const QSize size = QSize(qRound(width()), qRound(height()));
    if(size.isEmpty()) return;

    if(surface_.size() != size) {
        surface_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }
    paroculus::renderSketch(solution_, surface_.bits(), size.width(), size.height(),
                            static_cast<size_t>(surface_.bytesPerLine()));
    painter->drawImage(0, 0, surface_);
}
