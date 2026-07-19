// The only file in the project that knows about Qt and about the sketch core at
// the same time. Everything below it is toolkit-agnostic.
#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "sketch.h"

class SketchView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal ratio READ ratio WRITE setRatio NOTIFY solutionChanged)
    Q_PROPERTY(QString status READ status NOTIFY solutionChanged)

public:
    explicit SketchView(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    qreal ratio() const { return ratio_; }
    void setRatio(qreal ratio);
    QString status() const;

signals:
    void solutionChanged();

private:
    void resolve();

    qreal ratio_ = 1.618;  // golden section, len(A)/len(B)
    paroculus::Solution solution_;
    QImage surface_;  // retained so Skia is not handed a fresh buffer each frame
};
