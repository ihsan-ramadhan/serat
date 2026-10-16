#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

// LaTeX math drawn with the system's math font: fractions, roots, scripts, big operators, matrices, accents, symbols.
namespace MathRender {

struct Result {
    QImage image;       // device pixel ratio set; empty for empty input
    double baseline = 0; // from the top, in device-independent pixels
    QStringList errors;  // commands it does not know; they are drawn in red
};

Result render(const QString &tex, double pixelSize, const QColor &color, bool display, qreal dpr = 1.0);

}
