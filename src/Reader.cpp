#include "Reader.h"

#include "Diagrams.h"
#include "Editor.h"
#include "MathRender.h"
#include "PathActions.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QUrlQuery>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

Reader::Reader(QWidget *parent)
    : QTextBrowser(parent)
{
    setFrameShape(QFrame::NoFrame);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void Reader::mouseMoveEvent(QMouseEvent *e)
{
    QTextBrowser::mouseMoveEvent(e);
    QString href = e->modifiers() & Qt::ControlModifier ? anchorAt(e->position().toPoint()) : QString();
    if (!href.startsWith(u"wiki:"_s))
        href.clear();
    if (href != m_hover) {
        m_hover = href;
        Q_EMIT hoverLink(href, e->globalPosition().toPoint());
    }
}

int Reader::contentWidth() const
{
    return viewport()->width() - 2 * int(document()->documentMargin());
}

void Reader::resizeEvent(QResizeEvent *e)
{
    QTextBrowser::resizeEvent(e);
    const int scroll = verticalScrollBar()->isVisible() ? verticalScrollBar()->width() : 0;
    const int margin = readableMargin(width() - 2 * frameWidth() - scroll);
    setViewportMargins(margin, 4, margin, 0);
    if (contentWidth() != m_lastWidth) {
        m_lastWidth = contentWidth();
        Q_EMIT contentWidthChanged();
    }
}

QVariant Reader::loadResource(int type, const QUrl &name)
{
    const qreal dpr = m_scale > 0 ? m_scale : devicePixelRatioF();
    // "icon:<name>?color=%23rrggbb" from the renderer (callout icons).
    if (type == QTextDocument::ImageResource && name.scheme() == u"icon") {
        const QColor color(QUrlQuery(name).queryItemValue(u"color"_s, QUrl::FullyDecoded));
        return Theme::image(name.path(), color, 18, dpr);
    }
    if (type == QTextDocument::ImageResource && name.scheme() == u"diagram") {
        const QUrlQuery query(name);
        return Diagrams::instance().image(query.queryItemValue(u"src"_s, QUrl::FullyDecoded), query.queryItemValue(u"dark"_s) == u"1",
                                          dpr);
    }
    if (type == QTextDocument::ImageResource && name.scheme() == u"math") {
        const QUrlQuery query(name);
        const QFont font = document()->defaultFont();
        const double px = font.pixelSize() > 0 ? font.pixelSize() : font.pointSizeF() * logicalDpiY() / 72.0;
        return MathRender::render(query.queryItemValue(u"tex"_s, QUrl::FullyDecoded), px, palette().color(QPalette::Text),
                                  query.queryItemValue(u"d"_s) == u"1", dpr)
            .image;
    }
    return QTextBrowser::loadResource(type, name);
}

void Reader::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *menu = createStandardContextMenu(e->pos());
    // Qt's own "Copy Link Location" would copy the internal wiki: href.
    for (QAction *a : menu->actions())
        if (a->objectName() == u"link-copy")
            menu->removeAction(a);
    QAction *first = menu->actions().value(0);
    QList<QAction *> extra;
    auto add = [&](const QString &label, auto fn) {
        auto *a = new QAction(label, menu);
        connect(a, &QAction::triggered, this, fn);
        extra << a;
    };
    if (const QString href = anchorAt(e->pos()); !href.isEmpty() && !href.startsWith(u"task:"_s) && !href.startsWith(u"copy:"_s)) {
        add(tr("Open link"), [this, href] { Q_EMIT openRequested(href, false); });
        add(tr("Open link in new tab"), [this, href] { Q_EMIT openRequested(href, true); });
        add(tr("Copy link"), [href] { QApplication::clipboard()->setText(PathActions::hrefText(href)); });
    }
    QTextCursor c = cursorForPosition(e->pos());
    QTextCharFormat format = c.charFormat();
    if (!format.isImageFormat()) {
        c.movePosition(QTextCursor::Right);
        format = c.charFormat();
    }
    if (format.isImageFormat()) {
        const QString file = QUrl(format.toImageFormat().name()).toLocalFile();
        if (!file.isEmpty()) {
            add(tr("Open image"), [this, file] { Q_EMIT imageRequested(file, false); });
            add(tr("Open image in new tab"), [this, file] { Q_EMIT imageRequested(file, true); });
            add(tr("Copy image"), [file] { QApplication::clipboard()->setImage(QImage(file)); });
        }
    }
    if (!extra.isEmpty()) {
        menu->insertActions(first, extra);
        menu->insertSeparator(first);
    }
    menu->exec(e->globalPos());
    delete menu;
}

PreviewPopup::PreviewPopup(QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    , m_reader(new Reader(this))
{
    setObjectName(u"preview"_s);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->addWidget(m_reader);
    resize(460, 340);
}

void PreviewPopup::popup(const QString &html, const QString &anchor, const QPoint &globalPos)
{
    m_reader->setHtml(html);
    if (!anchor.isEmpty())
        m_reader->scrollToAnchor(anchor);
    QPoint at = globalPos + QPoint(12, 16);
    if (const QScreen *screen = QGuiApplication::screenAt(globalPos)) {
        const QRect area = screen->availableGeometry();
        if (at.x() + width() > area.right())
            at.setX(area.right() - width());
        if (at.y() + height() > area.bottom())
            at.setY(globalPos.y() - height() - 8);
    }
    move(at);
    show();
}

void PreviewPopup::leaveEvent(QEvent *e)
{
    QFrame::leaveEvent(e);
    hide();
}
