#include "EmbedBlocks.h"

#include "Reader.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

EmbedBlock::EmbedBlock(QWidget *parent)
    : QWidget(parent)
{
    setCursor(Qt::ArrowCursor);
}

void EmbedBlock::contextMenuEvent(QContextMenuEvent *e)
{
    if (menu)
        menu(e->globalPos());
}

void EmbedBlock::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && clicked)
        clicked();
    e->accept();
}

void ImageEmbed::setFile(const QString &file, const QSize &wanted)
{
    m_wanted = wanted;
    if (file == m_file)
        return;
    m_file = file;
    QImageReader reader(file);
    reader.setAutoTransform(true);
    m_natural = reader.size();
    // Keep big photos small in memory; the editor never shows them wider than this.
    if (m_natural.width() > 2000)
        reader.setScaledSize(m_natural.scaled(2000, m_natural.height(), Qt::KeepAspectRatio));
    m_image = reader.read();
    if (!m_natural.isValid())
        m_natural = m_image.size();
    m_scaled = {};
}

void ImageEmbed::setImage(const QImage &image)
{
    m_file.clear();
    m_image = image;
    m_natural = (QSizeF(image.size()) / image.devicePixelRatio()).toSize();
    m_wanted = {};
    m_scaled = {};
    update();
}

QSize ImageEmbed::sizeFor(int maxWidth) const
{
    QSize s = m_natural;
    if (s.isEmpty())
        return {};
    if (m_wanted.width() > 0 && m_wanted.height() > 0)
        s = s.scaled(m_wanted, Qt::KeepAspectRatio);
    else if (m_wanted.width() > 0)
        s = QSize(m_wanted.width(), qMax(1, qRound(s.height() * double(m_wanted.width()) / s.width())));
    if (s.width() > maxWidth)
        s = QSize(maxWidth, qMax(1, qRound(s.height() * double(maxWidth) / s.width())));
    return s;
}

void ImageEmbed::paintEvent(QPaintEvent *)
{
    const qreal dpr = devicePixelRatioF();
    const QSize px = size() * dpr;
    if (m_scaled.size() != px) {
        m_scaled = QPixmap::fromImage(m_image.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        m_scaled.setDevicePixelRatio(dpr);
    }
    QPainter(this).drawPixmap(0, 0, m_scaled);
}

NoteEmbed::NoteEmbed(QWidget *parent)
    : EmbedBlock(parent)
    , m_title(new QLabel(this))
    , m_reader(new Reader(this))
{
    setObjectName(u"noteEmbed"_s);
    setAttribute(Qt::WA_StyledBackground);
    m_title->setObjectName(u"noteEmbedTitle"_s);
    m_title->setCursor(Qt::PointingHandCursor);
    m_reader->setFocusPolicy(Qt::NoFocus);
    m_reader->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 6, 6, 6);
    layout->setSpacing(2);
    layout->addWidget(m_title);
    layout->addWidget(m_reader, 1);
    connect(m_title, &QLabel::linkActivated, this, [this] {
        if (open)
            open(QApplication::keyboardModifiers() & Qt::ControlModifier);
    });
}

void NoteEmbed::setContent(const QString &title, const QString &html, const QFont &font, const QString &styleSheet)
{
    m_title->setText(u"<a href=\"open\">%1</a>"_s.arg(title.toHtmlEscaped()));
    m_reader->document()->setDefaultFont(font);
    m_reader->document()->setDefaultStyleSheet(styleSheet);
    m_reader->setHtml(html);
}

// As tall as the note, up to a limit; longer notes scroll inside.
QSize NoteEmbed::sizeFor(int maxWidth) const
{
    QTextDocument *doc = m_reader->document();
    doc->setTextWidth(maxWidth - 30);
    const int body = qMin(int(doc->size().height()) + 8, 420);
    return QSize(maxWidth, m_title->sizeHint().height() + body + 16);
}
