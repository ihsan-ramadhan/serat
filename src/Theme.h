#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>

class QApplication;

// Colours come from the app icon: slate ink, warm paper, gold thread.
namespace Theme {

struct Colors {
    bool dark;
    QColor window;
    QColor base;
    QColor raised;
    QColor border;
    QColor hover;
    QColor text;
    QColor muted;
    QColor accent;
    QColor selection;
};

void install(QApplication &app);

// "system", "light" or "dark"; stored in the settings.
QString preference();
void setPreference(const QString &mode);
const Colors &colors();

// :/icons/<name>.svg tinted to the theme; openName is drawn for the On state.
QIcon icon(const QString &name, const QString &openName = {});
QImage image(const QString &name, const QColor &color, int size, qreal dpr);

}
