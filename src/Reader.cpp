#include "Reader.h"

#include "Editor.h"

#include <QScrollBar>

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
