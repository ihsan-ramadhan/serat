#include "Reader.h"

#include "Editor.h"
#include "PathActions.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QUrlQuery>

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
    // "icon:<name>?color=%23rrggbb" from the renderer (callout icons).
    if (type == QTextDocument::ImageResource && name.scheme() == u"icon") {
        const QColor color(QUrlQuery(name).queryItemValue(u"color"_s, QUrl::FullyDecoded));
        return Theme::image(name.path(), color, 18, devicePixelRatioF());
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
