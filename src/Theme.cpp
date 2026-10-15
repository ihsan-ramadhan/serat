#include "Theme.h"

#include "Markdown.h"

#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QSettings>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>

using namespace Qt::StringLiterals;

namespace Theme {

namespace {

Colors g_colors;
QPalette g_systemPalette;

Colors makeColors(bool dark)
{
    Colors c;
    c.dark = dark;
    if (dark) {
        c.window = QColor(u"#17191e"_s);
        c.base = QColor(u"#1e2026"_s);
        c.raised = QColor(u"#25282f"_s);
        c.border = QColor(u"#2e313a"_s);
        c.hover = QColor(u"#272a31"_s);
        c.text = QColor(u"#e7e3da"_s);
        c.muted = QColor(u"#9c988f"_s);
        c.accent = QColor(u"#e8c26b"_s);
        c.selection = mix(c.base, c.accent, 0.25);
    } else {
        c.window = QColor(u"#f0ede5"_s);
        c.base = QColor(u"#fbfaf6"_s);
        c.raised = QColor(u"#f5f2ea"_s);
        c.border = QColor(u"#ddd8cb"_s);
        c.hover = QColor(u"#e7e3d9"_s);
        c.text = QColor(u"#2b2f3a"_s);
        c.muted = QColor(u"#6a675f"_s);
        c.accent = QColor(u"#8a5a00"_s);
        c.selection = mix(c.base, QColor(u"#e8c26b"_s), 0.4);
    }
    return c;
}

QPalette makePalette(const Colors &c)
{
    QPalette p;
    auto both = [&p](QPalette::ColorRole role, const QColor &color) { p.setColor(role, color); };
    both(QPalette::Window, c.window);
    both(QPalette::WindowText, c.text);
    both(QPalette::Base, c.base);
    both(QPalette::AlternateBase, c.raised);
    both(QPalette::Text, c.text);
    both(QPalette::Button, c.raised);
    both(QPalette::ButtonText, c.text);
    both(QPalette::BrightText, c.text);
    both(QPalette::Highlight, c.selection);
    both(QPalette::HighlightedText, c.text);
    both(QPalette::Link, c.accent);
    both(QPalette::LinkVisited, c.accent);
    both(QPalette::PlaceholderText, c.muted);
    both(QPalette::ToolTipBase, c.raised);
    both(QPalette::ToolTipText, c.text);
    both(QPalette::Light, c.raised);
    both(QPalette::Midlight, c.hover);
    both(QPalette::Mid, c.border);
    both(QPalette::Dark, c.border);
    both(QPalette::Shadow, c.window);
    both(QPalette::Accent, c.accent);
    const QColor disabled = mix(c.muted, c.base, 0.35);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

QString styleSheet(const Colors &c)
{
    const QString scroll = mix(c.base, c.text, c.dark ? 0.22 : 0.25).name();
    const QString scrollHover = mix(c.base, c.text, c.dark ? 0.35 : 0.4).name();
    const QString pressed = mix(c.hover, c.text, 0.08).name();
    return uR"(
QMainWindow, QDialog { background: %1; }
QMenuBar { background: %1; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; border-radius: 5px; background: transparent; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: %5; }
QMenu { background: %3; border: 1px solid %4; padding: 4px; }
QMenu::item { padding: 5px 28px 5px 12px; border-radius: 5px; }
QMenu::item:selected { background: %9; }
QMenu::item:disabled { color: %7; }
QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }
QToolTip { background: %3; color: %6; border: 1px solid %4; padding: 4px 6px; }

QTabWidget::pane { border: none; border-top: 1px solid %4; }
QTabBar { background: %1; }
QTabBar::tab { background: transparent; color: %7; border: none; border-bottom: 2px solid transparent;
               padding: 7px 10px 6px 12px; margin: 0px; }
QTabBar::tab:hover { color: %6; background: %5; }
QTabBar::tab:selected { color: %6; border-bottom: 2px solid %8; }
QTabBar#notes::tab { min-width: 90px; max-width: 220px; }
QTabBar#panels::tab { padding: 7px 10px 6px 10px; }
QTabBar QToolButton { border-radius: 4px; padding: 1px; margin: 0px; }
QTabBar::scroller { width: 44px; }

QSplitter::handle { background: %4; }
QStatusBar { background: %1; color: %7; border-top: 1px solid %4; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: %7; padding: 0px 8px; }
QStatusBar QToolButton#statusButton { color: %7; padding: 0px 8px; border-radius: 4px; font-weight: 400; }
QStatusBar QToolButton#statusButton:hover { color: %6; }
QLabel#emptyTitle { color: %7; font-size: 15pt; }
QPushButton#emptyAction { background: transparent; border: none; color: %8; padding: 4px 8px; }
QPushButton#emptyAction:hover { text-decoration: underline; }
QPushButton#emptyAction:focus { border: 1px solid %8; }
QFrame#preview { background: %2; border: 1px solid %4; border-radius: 8px; }

QTreeView, QListView { background: %1; border: none; show-decoration-selected: 0; selection-background-color: %9; selection-color: %6; }
QTreeView::item, QListView::item { padding: 3px 4px; border-radius: 5px; }
QTreeView::item:hover, QListView::item:hover { background: %5; }
QTreeView::item:selected, QListView::item:selected { background: %9; color: %6; }
QTreeView::item { border-radius: 0px; } /* the branch area is tinted too, so keep one flat bar */
QListWidget#switcher { background: %2; border: 1px solid %4; border-radius: 8px; padding: 4px; }
QListWidget#switcher::item { padding: 6px 8px; }

QLineEdit { background: %2; color: %6; border: 1px solid %4; border-radius: 6px; padding: 5px 8px;
            selection-background-color: %9; selection-color: %6; }
QLineEdit:focus { border: 1px solid %8; }
QPlainTextEdit, QTextBrowser { selection-background-color: %9; selection-color: %6; }

QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %7; border-radius: 4px; background: %2; }
QCheckBox::indicator:hover { border: 1px solid %6; }
QCheckBox::indicator:checked { background: %8; border: 1px solid %8; image: url(:/icons/%13.svg); }

QPushButton { background: %3; color: %6; border: 1px solid %4; border-radius: 6px; padding: 6px 14px; }
QPushButton:hover { background: %5; }
QPushButton:pressed { background: %12; }
QPushButton:focus { border: 1px solid %8; }
QPushButton#primary { background: #e8c26b; color: #2b2f3a; border: 1px solid #e8c26b; font-weight: 600; }
QPushButton#primary:hover { background: #efcf86; }
QPushButton#primary:focus { border: 2px solid %6; }
QPushButton#vaultRow { background: transparent; border: 1px solid transparent; text-align: left; padding: 0px; }
QPushButton#vaultRow:hover { background: %5; }
QPushButton#vaultRow:focus { border: 1px solid %8; }

QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 3px; }
QToolButton:hover { background: %5; }
QToolButton:pressed, QToolButton:checked { background: %12; }
QToolButton:focus { border: 1px solid %8; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0px; }
QScrollBar::handle { background: %10; border-radius: 3px; margin: 2px; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::handle:hover { background: %11; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTableView#mdTable { background: %2; color: %6; border: none; border-top: 1px solid %4; border-left: 1px solid %4;
                     gridline-color: %4;
                     selection-background-color: %9; selection-color: %6; }
QTableView#mdTable QLineEdit { border: 1px solid %8; border-radius: 0px; padding: 0px 7px; background: %2; } /* lines up with TextInset */
QToolButton#tableAdd { border-radius: 4px; }

QFrame#properties { background: %2; border: none; }
QFrame#properties QToolButton { font-weight: 400; }
QFrame#properties QToolButton#propertiesHeader { font-weight: 600; padding: 3px 6px 3px 2px; }
QFrame#properties QLineEdit { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; }
QFrame#properties QLineEdit:hover { background: %5; }
QFrame#properties QLineEdit:focus { background: %2; border: 1px solid %8; }
QLineEdit#propertyKey { color: %7; }
QLabel#propertyRaw { color: %7; padding: 3px 6px; }
QFrame#pill { background: %3; border: 1px solid %4; border-radius: 10px; }
QToolButton#pillRemove { padding: 1px; border-radius: 7px; }
)"_s.arg(c.window.name(), c.base.name(), c.raised.name(), c.border.name(), c.hover.name(), c.text.name(),
           c.muted.name(), c.accent.name(), c.selection.name())
        .arg(scroll, scrollHover, pressed, c.dark ? u"check-mark"_s : u"check-mark-light"_s);
}

// A dark platform palette wins when the portal reports no preference.
bool systemIsDark(const QPalette &system)
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
           || system.color(QPalette::Window).lightness() < 128;
}

QByteArray loadSvg(const QString &name)
{
    QFile f(u":/icons/"_s + name + u".svg"_s);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

class TintedIcon : public QIconEngine
{
public:
    explicit TintedIcon(const QString &name, const QString &openName = {})
        : m_name(name)
        , m_openName(openName)
        , m_svg(loadSvg(name))
        , m_openSvg(openName.isEmpty() ? QByteArray() : loadSvg(openName))
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const Colors &c = g_colors;
        const bool open = state == QIcon::On && !m_openSvg.isEmpty();
        QColor color = c.muted;
        if (mode == QIcon::Disabled)
            color = mix(c.muted, c.window, 0.5);
        else if (mode == QIcon::Active || mode == QIcon::Selected || (state == QIcon::On && !open))
            color = c.text;
        QByteArray svg = open ? m_openSvg : m_svg;
        svg.replace("currentColor", color.name().toLatin1());
        QSvgRenderer renderer(svg);
        renderer.render(painter, QRectF(rect));
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(scale);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(), size), mode, state);
        return pm;
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QIconEngine *clone() const override { return new TintedIcon(m_name, m_openName); }
    QString key() const override { return u"serat-tinted"_s; }

private:
    QString m_name, m_openName;
    QByteArray m_svg, m_openSvg;
};

// Fusion without the desktop icon theme in places that clash with the app's own icons.
class Style : public QProxyStyle
{
public:
    Style()
        : QProxyStyle(QStyleFactory::create(u"Fusion"_s))
    {
    }

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *ret) const override
    {
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }

    QIcon standardIcon(StandardPixmap icon, const QStyleOption *option, const QWidget *widget) const override
    {
        switch (icon) {
        case SP_LineEditClearButton: return Theme::icon(u"close"_s);
        case SP_MessageBoxQuestion: return Theme::icon(u"help"_s);
        case SP_MessageBoxInformation: return Theme::icon(u"info"_s);
        case SP_MessageBoxWarning:
        case SP_MessageBoxCritical: return Theme::icon(u"alert"_s);
        default: break;
        }
        return QProxyStyle::standardIcon(icon, option, widget);
    }
};

void apply()
{
    const QString pref = preference();
    const bool dark = pref == u"dark" || (pref != u"light" && systemIsDark(g_systemPalette));
    g_colors = makeColors(dark);
    qApp->setPalette(makePalette(g_colors));
    qApp->setStyleSheet(styleSheet(g_colors));
}

} // namespace

void install(QApplication &app)
{
    g_systemPalette = app.palette();
    app.setStyle(new Style);
    apply();
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [] { apply(); });
}

QString preference()
{
    return QSettings().value(u"theme"_s, u"system"_s).toString();
}

void setPreference(const QString &mode)
{
    QSettings().setValue(u"theme"_s, mode);
    apply();
}

const Colors &colors()
{
    return g_colors;
}

QIcon icon(const QString &name, const QString &openName)
{
    return QIcon(new TintedIcon(name, openName));
}

QImage image(const QString &name, const QColor &color, int size, qreal dpr)
{
    QFile f(u":/icons/"_s + name + u".svg"_s);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QImage img(QSize(size, size) * dpr, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    img.setDevicePixelRatio(dpr);
    QPainter p(&img);
    QSvgRenderer(f.readAll().replace("currentColor", color.name().toLatin1())).render(&p, QRectF(0, 0, size, size));
    return img;
}

} // namespace Theme
