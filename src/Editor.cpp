#include "Editor.h"

#include "Markdown.h"
#include "MarkdownHighlighter.h"
#include "PathActions.h"
#include "Properties.h"
#include "TableEditor.h"
#include "Theme.h"
#include "Vault.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QAbstractTextDocumentLayout>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QScopedValueRollback>
#include <QPainter>
#include <QTextLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QStringListModel>
#include <QTextBlock>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace {

const QRegularExpression &listRe()
{
    static const QRegularExpression re(u"^(\\s*)(([-*+])|(\\d+)([.)]))(\\s+)(\\[[ xX]\\]\\s+)?"_s);
    return re;
}

QString wikiHref(const QString &target)
{
    return u"wiki:"_s + QString::fromUtf8(QUrl::toPercentEncoding(target));
}

bool fencedOrFrontmatter(const QTextBlock &b)
{
    const int st = b.userState();
    return st > 0 && (st & (BlockState::InFence | BlockState::Frontmatter));
}

const QRegularExpression &calloutHeadRe()
{
    static const QRegularExpression re(u"^\\s*>\\s*\\[!([\\w-]+)\\][+-]? *(.*)$"_s);
    return re;
}

} // namespace

// An image embed drawn over its line in live preview.
class ImageBlock : public QWidget
{
public:
    explicit ImageBlock(QWidget *parent)
        : QWidget(parent)
    {
        setCursor(Qt::ArrowCursor);
    }

    QString path; // vault-relative
    std::function<void(const QPoint &)> menu;
    std::function<void()> clicked;

    void setImage(const QString &file, const QSize &wanted)
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

    bool isNull() const { return m_image.isNull(); }

    QSize sizeFor(int maxWidth) const
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

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const QSize px = size() * dpr;
        if (m_scaled.size() != px) {
            m_scaled = QPixmap::fromImage(m_image.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            m_scaled.setDevicePixelRatio(dpr);
        }
        QPainter(this).drawPixmap(0, 0, m_scaled);
    }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        if (menu)
            menu(e->globalPos());
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && clicked)
            clicked();
        e->accept();
    }

private:
    QString m_file;
    QImage m_image;
    QSize m_natural, m_wanted;
    QPixmap m_scaled;
};

int readableMargin(int available, int maxWidth, int minMargin)
{
    return qMax(minMargin, (available - maxWidth) / 2);
}

Editor::Editor(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_highlighter(new MarkdownHighlighter(document()))
    , m_completer(new QCompleter(this))
    , m_candidates(new QStringListModel(this))
{
    setFrameShape(QFrame::NoFrame);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    viewport()->setMouseTracking(true);

    m_completer->setModel(m_candidates);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setMaxVisibleItems(10);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &Editor::insertCompletion);


    m_tableTimer.setSingleShot(true);
    m_tableTimer.setInterval(0);
    connect(&m_tableTimer, &QTimer::timeout, this, &Editor::scanTables);
    connect(document(), &QTextDocument::contentsChange, this, [this](int pos, int removed, int added) {
        if (m_inKeyPress)
            m_typedBlock = document()->findBlock(pos).blockNumber();
        if ((removed || added) && !m_tableBusy)
            m_tableTimer.start();
    });
    connect(this, &QPlainTextEdit::updateRequest, this, &Editor::positionTables);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &Editor::onCursorMoved);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &Editor::updateRawBlocks);
    connect(this, &QPlainTextEdit::selectionChanged, this, &Editor::updateRawBlocks);
    retheme();
}

void Editor::setLinkCandidates(const QStringList &names)
{
    // Resetting the model on every autosave would drop the popup's selected row.
    if (names == m_linkNames)
        return;
    m_linkNames = names;
    if (!m_headingMode)
        m_candidates->setStringList(names);
    if (m_completer->popup()->isVisible())
        updateCompletion();
}

void Editor::applyFont(const QFont &font)
{
    setFont(font);
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(u' ') * 4);
    retheme();
    for (const Table &t : std::as_const(m_tables))
        t.overlay->setNoteFont(font);
    layoutTables();
}

void Editor::retheme()
{
    const QPalette pal = palette();
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    m_codeBg = mix(pal.color(QPalette::Base), pal.color(QPalette::Text), dark ? 0.08 : 0.05);
    m_highlighter->setTheme(pal, font());
}

void Editor::changeEvent(QEvent *e)
{
    QPlainTextEdit::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        retheme();
}

void Editor::goToLine(int line)
{
    const QTextBlock block = document()->findBlockByNumber(line);
    if (!block.isValid())
        return;
    setTextCursor(QTextCursor(block));
    centerCursor();
    setFocus();
}

void Editor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    updateMargins();
    layoutTables();
}

void Editor::updateMargins()
{
    const int scroll = verticalScrollBar()->isVisible() ? verticalScrollBar()->width() : 0;
    const int margin = readableMargin(width() - 2 * frameWidth() - scroll);
    setViewportMargins(margin, 4, margin, 0);
}

void Editor::paintEvent(QPaintEvent *e)
{
    {
        QPainter p(viewport());
        const QPointF offset = contentOffset();
        for (QTextBlock b = firstVisibleBlock(); b.isValid(); b = b.next()) {
            const QRectF r = blockBoundingGeometry(b).translated(offset);
            if (r.top() > viewport()->height())
                break;
            const int state = b.userState();
            if (state > 0 && (state & (BlockState::InFence | BlockState::FenceLine)))
                p.fillRect(QRectF(0, r.top(), viewport()->width(), r.height()), m_codeBg);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        const double left = document()->documentMargin();
        for (QTextBlock b = m_live ? firstVisibleBlock() : QTextBlock(); b.isValid();) {
            const QRectF r = blockBoundingGeometry(b).translated(offset);
            if (r.top() > viewport()->height())
                break;
            const auto [first, last] = calloutAt(b);
            if (!first.isValid()) {
                b = b.next();
                continue;
            }
            if (m_highlighter->hidesMarkup(first.blockNumber())) {
                const double top = blockBoundingGeometry(first).translated(offset).top() - 2;
                const double bottom = blockBoundingGeometry(last).translated(offset).bottom() + 2;
                const QColor color(calloutStyle(calloutHeadRe().match(first.text()).captured(1)).color);
                p.setBrush(mix(palette().color(QPalette::Base), color, dark ? 0.14 : 0.09));
                p.drawRoundedRect(QRectF(left, top, viewport()->width() - 2 * left, bottom - top), 6, 6);
            }
            b = last.next();
        }
    }
    QPlainTextEdit::paintEvent(e);
    if (!m_live)
        return;

    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QColor muted = pal.color(QPalette::PlaceholderText);
    const QColor bar = mix(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.25);
    const QPointF offset = contentOffset();
    for (QTextBlock b = firstVisibleBlock(); b.isValid(); b = b.next()) {
        if (blockBoundingGeometry(b).translated(offset).top() > viewport()->height())
            break;
        if (!previewed(b))
            continue;
        if (b.userState() > 0 && (b.userState() & BlockState::Callout))
            paintCalloutHead(p, b);
        for (const Deco &d : decorations(b)) {
            switch (d.kind) {
            case Deco::Quote:
                p.fillRect(d.rect, bar);
                break;
            case Deco::Rule:
                p.fillRect(QRectF(d.rect.left(), d.rect.center().y(), d.rect.width(), 1), bar);
                break;
            case Deco::Bullet: {
                const double r = qMax(2.5, d.rect.height() * 0.11);
                p.setPen(Qt::NoPen);
                p.setBrush(muted);
                p.drawEllipse(d.rect.center(), r, r);
                break;
            }
            case Deco::Task: {
                const double side = qMin(d.rect.height() * 0.62, 16.0);
                const QRectF box(d.rect.center().x() - side / 2, d.rect.center().y() - side / 2, side, side);
                if (d.checked) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(pal.color(QPalette::Link));
                    p.drawRoundedRect(box, 3, 3);
                    p.setPen(QPen(pal.color(QPalette::Base), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p.setBrush(Qt::NoBrush);
                    p.drawPolyline(QList<QPointF>{box.topLeft() + QPointF(side * 0.24, side * 0.52),
                                                  box.topLeft() + QPointF(side * 0.43, side * 0.7),
                                                  box.topLeft() + QPointF(side * 0.76, side * 0.32)});
                } else {
                    p.setPen(QPen(muted, 1.5));
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
                }
                break;
            }
            }
        }
    }
}

void Editor::setLivePreview(bool on)
{
    if (on == m_live)
        return;
    m_live = on;
    m_highlighter->setLivePreview(on);
    m_highlighter->rehighlight();
    scanTables();
    viewport()->update();
}

void Editor::updateRawBlocks()
{
    const QTextCursor c = textCursor();
    const QTextBlock first = document()->findBlock(c.selectionStart());
    const QTextBlock last = document()->findBlock(c.selectionEnd());
    int from = first.blockNumber(), to = last.blockNumber();
    // A callout shows its source as a whole while the cursor is in it.
    if (m_live) {
        if (const QTextBlock head = calloutAt(first).first; head.isValid())
            from = head.blockNumber();
        if (const QTextBlock end = calloutAt(last).second; end.isValid())
            to = end.blockNumber();
    }
    const int colFrom = c.selectionStart() - first.position(), colTo = c.selectionEnd() - last.position();
    if (from == m_rawFrom && to == m_rawTo && colFrom == m_colFrom && colTo == m_colTo)
        return;
    const int oldFrom = m_rawFrom, oldTo = m_rawTo;
    m_rawFrom = from;
    m_rawTo = to;
    m_colFrom = colFrom;
    m_colTo = colTo;
    m_highlighter->setRawRange(from, colFrom, to, colTo);
    updateTableHint();
    if (!m_live)
        return;
    auto refresh = [this](int a, int b) {
        for (QTextBlock blk = document()->findBlockByNumber(a); blk.isValid() && blk.blockNumber() <= b; blk = blk.next())
            m_highlighter->rehighlightBlock(blk);
    };
    if (oldFrom >= 0)
        refresh(oldFrom, oldTo);
    refresh(from, to);
    viewport()->update();
}

// Markdown makes text right under a table one of its rows; warn while it is typed.
void Editor::updateTableHint()
{
    QString hint;
    const QTextBlock b = textCursor().block();
    if (m_rawTable.isValid()) {
        const int row = b.blockNumber() - m_rawTable.blockNumber();
        if (row >= 2 && row < m_rawLines && !MdTable::hasPipe(b.text()) && !b.text().trimmed().isEmpty())
            hint = tr("This line is part of the table above. Leave an empty line before it to start a paragraph.");
    }
    if (hint != m_hint) {
        m_hint = hint;
        Q_EMIT hintChanged(hint);
    }
}

bool Editor::previewed(const QTextBlock &block) const
{
    if (!m_live || !block.isVisible() || !m_highlighter->hidesMarkup(block.blockNumber()))
        return false;
    if (const auto *t = static_cast<const TableLineData *>(block.userData()); t && t->pixelSize)
        return false;
    const auto inside = [](int state) {
        return state > 0 && (state & (BlockState::InFence | BlockState::FenceLine | BlockState::Frontmatter));
    };
    return !inside(block.userState()) && !inside(block.previous().userState());
}

QList<Editor::Deco> Editor::decorations(const QTextBlock &block) const
{
    static const QRegularExpression hr(u"^ {0,3}([-*_])( *\\1){2,} *$"_s);
    static const QRegularExpression quote(u"^\\s*(>[ >]*)"_s);
    static const QRegularExpression list(u"^\\s*([-*+])\\s+(\\[[ xX]\\]\\s)?"_s);
    QList<Deco> out;
    QTextLayout *layout = block.layout();
    if (!layout || layout->lineCount() == 0)
        return out;
    const QString text = block.text();
    const QRectF r = blockBoundingGeometry(block).translated(contentOffset());
    auto chars = [&](int i, int n) {
        const QTextLine line = layout->lineForTextPosition(i);
        const double x1 = line.cursorToX(i), x2 = line.cursorToX(i + n);
        return QRectF(r.left() + x1, r.top() + line.y(), x2 - x1, line.height());
    };

    if (hr.match(text).hasMatch()) {
        const double left = r.left() + document()->documentMargin();
        out.append({Deco::Rule, QRectF(left, r.top(), viewport()->width() - 2 * left, r.height())});
        return out;
    }
    int from = 0;
    const bool callout = block.userState() > 0 && (block.userState() & BlockState::Callout);
    if (const auto m = quote.match(text); m.hasMatch()) {
        for (int i = int(m.capturedStart(1)); i < m.capturedEnd(1) && !callout; ++i)
            if (text[i] == u'>')
                out.append({Deco::Quote, QRectF(chars(i, 1).left() + 1, r.top(), 3, r.height())});
        from = int(m.capturedEnd(1));
    }
    const auto m = list.match(text, from, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (m.hasMatch()) {
        if (m.capturedLength(2)) {
            const int at = int(m.capturedStart(2));
            out.append({Deco::Task, chars(at, 3), text[at + 1] != u' ', at + 1});
        } else {
            out.append({Deco::Bullet, chars(int(m.capturedStart(1)), 1)});
        }
    }
    return out;
}

QString Editor::clickedLink(const QPoint &pos) const
{
    const QTextCursor c = cursorForPosition(pos);
    if (!previewed(c.block()))
        return {};
    if (c.atBlockEnd() && pos.x() > cursorRect(c).right() + 2)
        return {}; // past the end of the line
    return linkAt(c);
}

void Editor::keyPressEvent(QKeyEvent *e)
{
    const QScopedValueRollback typing(m_inKeyPress, true);
    if (m_completer->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            e->ignore();
            return;
        default:
            break;
        }
    }

    const auto mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End: {
        m_navKey = true;
        m_navUp = e->key() == Qt::Key_Up || e->key() == Qt::Key_Left || e->key() == Qt::Key_PageUp
                  || e->key() == Qt::Key_Home;
        QPlainTextEdit::keyPressEvent(e);
        m_navKey = false;
        return;
    }
    default:
        break;
    }

    QTextCursor c = textCursor();
    const QChar prev = document()->characterAt(c.position() - 1);
    const QChar next = document()->characterAt(c.position());

    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && mods == Qt::NoModifier && continueList())
        return;
    if (e->key() == Qt::Key_Tab && mods == Qt::NoModifier) {
        const bool multiLine = c.hasSelection() && document()->findBlock(c.selectionStart()) != document()->findBlock(c.selectionEnd());
        if (multiLine || listRe().match(c.block().text()).hasMatch()) {
            indentLines(false);
            return;
        }
    }
    if (e->key() == Qt::Key_Backtab) {
        indentLines(true);
        return;
    }
    if (e->text() == u"[") {
        if (c.hasSelection()) {
            c.insertText(u'[' + c.selectedText() + u']');
        } else {
            c.insertText(u"[]"_s);
            c.movePosition(QTextCursor::Left);
            setTextCursor(c);
        }
        updateCompletion();
        return;
    }
    if (e->text() == u"]" && !c.hasSelection() && next == u']') {
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
        updateCompletion();
        return;
    }
    if (e->key() == Qt::Key_Backspace && mods == Qt::NoModifier && !c.hasSelection() && prev == u'[' && next == u']') {
        c.beginEditBlock();
        c.deletePreviousChar();
        c.deleteChar();
        c.endEditBlock();
        updateCompletion();
        return;
    }

    QPlainTextEdit::keyPressEvent(e);
    updateCompletion();
}

bool Editor::continueList()
{
    QTextCursor c = textCursor();
    if (c.hasSelection())
        return false;
    const QString line = c.block().text();
    const int col = c.positionInBlock();

    if (const auto m = listRe().match(line); m.hasMatch() && col >= m.capturedEnd()) {
        if (line.mid(m.capturedEnd()).trimmed().isEmpty() && col == line.size()) {
            // Enter on an empty item: outdent, or end the list at top level.
            if (!m.captured(1).isEmpty()) {
                indentLines(true);
            } else {
                c.select(QTextCursor::BlockUnderCursor);
                c.movePosition(QTextCursor::StartOfBlock);
                c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                c.removeSelectedText();
            }
            return true;
        }
        const QString marker = m.captured(3).isEmpty() ? QString::number(m.captured(4).toInt() + 1) + m.captured(5)
                                                       : m.captured(3);
        c.insertText(u'\n' + m.captured(1) + marker + m.captured(6) + (m.capturedLength(7) ? u"[ ] "_s : QString()));
        setTextCursor(c);
        ensureCursorVisible();
        return true;
    }

    static const QRegularExpression quote(u"^(\\s*>[> ]*)"_s);
    if (const auto m = quote.match(line); m.hasMatch() && col >= m.capturedEnd()) {
        if (line.mid(m.capturedEnd()).trimmed().isEmpty()) {
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            return true;
        }
        c.insertText(u'\n' + m.captured(1));
        setTextCursor(c);
        ensureCursorVisible();
        return true;
    }
    return false;
}

void Editor::indentLines(bool outdent)
{
    // Indent like the note does: tabs (Obsidian's default) or its first space indent.
    QString unit = u"\t"_s;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const auto m = listRe().match(b.text());
        if (m.hasMatch() && !m.captured(1).isEmpty()) {
            const QString ws = m.captured(1);
            if (ws[0] == u' ')
                unit = QString(qBound(2, int(ws.size()), 4), u' ');
            break;
        }
    }

    QTextCursor c = textCursor();
    QTextBlock b = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(c.selectionEnd());
    if (c.hasSelection() && last != b && c.selectionEnd() == last.position())
        last = last.previous();

    c.beginEditBlock();
    for (;; b = b.next()) {
        QTextCursor lc(b);
        if (outdent) {
            const QString t = b.text();
            int n = 0;
            if (t.startsWith(u'\t'))
                n = 1;
            else
                while (n < unit.size() && n < t.size() && t[n] == u' ')
                    ++n;
            lc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n);
            lc.removeSelectedText();
        } else {
            lc.insertText(unit);
        }
        if (b == last)
            break;
    }
    c.endEditBlock();
}

void Editor::updateCompletion()
{
    const QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const qsizetype open = before.lastIndexOf(u"[["_s);
    QAbstractItemView *popup = m_completer->popup();
    if (c.hasSelection() || open < 0 || before.indexOf(u"]]"_s, open) >= 0) {
        popup->hide();
        return;
    }
    QString prefix = before.mid(open + 2);
    if (prefix.contains(u'|') || prefix.size() > 100) {
        popup->hide();
        return;
    }
    // [[#Heading in this note, [[Note#Heading in another one.
    const qsizetype hash = prefix.indexOf(u'#');
    m_headingMode = hash >= 0;
    QStringList names = m_linkNames;
    if (m_headingMode) {
        const QString note = prefix.left(hash);
        prefix = prefix.mid(hash + 1);
        const QString path = note.isEmpty() || !m_vault ? m_notePath : m_vault->resolve(note, m_notePath);
        const Note *n = m_vault ? m_vault->note(path) : nullptr;
        names.clear();
        if (n)
            for (const Heading &h : n->headings)
                names << h.text;
    }
    if (names != m_candidates->stringList())
        m_candidates->setStringList(names);
    if (prefix != m_completer->completionPrefix())
        m_completer->setCompletionPrefix(prefix);
    if (m_completer->completionCount() == 0) {
        popup->hide();
        return;
    }
    QRect r = cursorRect().translated(viewport()->geometry().topLeft());
    r.setWidth(qMax(300, popup->sizeHintForColumn(0) + popup->verticalScrollBar()->sizeHint().width() + 24));
    m_completer->complete(r);
    // complete() clears the current row; select the best match so Enter takes it.
    popup->setCurrentIndex(m_completer->completionModel()->index(0, 0));
}

void Editor::insertCompletion(const QString &name)
{
    QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const qsizetype open = before.lastIndexOf(u"[["_s);
    if (open < 0)
        return;
    const qsizetype hash = before.indexOf(u'#', open);
    c.beginEditBlock();
    c.setPosition(c.block().position() + int(m_headingMode && hash >= 0 ? hash + 1 : open + 2), QTextCursor::KeepAnchor);
    c.insertText(name);
    if (document()->characterAt(c.position()) == u']' && document()->characterAt(c.position() + 1) == u']')
        c.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, 2);
    else
        c.insertText(u"]]"_s);
    c.endEditBlock();
    setTextCursor(c);
}

QString Editor::linkAt(const QTextCursor &cursor) const
{
    static const QRegularExpression wiki(u"!?\\[\\[([^\\[\\]\\n]+)\\]\\]"_s);
    static const QRegularExpression md(u"\\[[^\\]\\n]*\\]\\(([^)\\s]+)\\)"_s);
    static const QRegularExpression url(u"https?://[^\\s)>\\]]+"_s);
    const QString line = cursor.block().text();
    const int col = cursor.positionInBlock();
    auto covers = [col](const QRegularExpressionMatch &m) { return col >= m.capturedStart() && col <= m.capturedEnd(); };

    for (auto it = wiki.globalMatch(line); it.hasNext();) {
        const auto m = it.next();
        if (covers(m))
            return wikiHref(m.captured(1).section(u'|', 0, 0).trimmed());
    }
    for (auto it = md.globalMatch(line); it.hasNext();) {
        const auto m = it.next();
        if (!covers(m))
            continue;
        const QString href = m.captured(1);
        return QUrl(href).scheme().isEmpty() ? wikiHref(QUrl::fromPercentEncoding(href.toUtf8())) : href;
    }
    for (auto it = url.globalMatch(line); it.hasNext();) {
        const auto m = it.next();
        if (covers(m))
            return m.captured(0);
    }
    return {};
}

void Editor::mousePressEvent(QMouseEvent *e)
{
    if (m_live && e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        const QTextBlock b = cursorForPosition(e->position().toPoint()).block();
        if (previewed(b)) {
            for (const Deco &d : decorations(b)) {
                if (d.kind == Deco::Task && d.rect.adjusted(-3, 0, 3, 0).contains(e->position())) {
                    QTextCursor t(document());
                    t.setPosition(b.position() + d.pos);
                    t.setPosition(b.position() + d.pos + 1, QTextCursor::KeepAnchor);
                    t.insertText(d.checked ? u" "_s : u"x"_s);
                    e->accept();
                    return;
                }
            }
        }
        if (const QString href = clickedLink(e->position().toPoint()); !href.isEmpty()) {
            Q_EMIT linkActivated(href, false);
            e->accept();
            return;
        }
    }
    const bool follow = (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
                        || e->button() == Qt::MiddleButton;
    if (follow) {
        const QString href = linkAt(cursorForPosition(e->position().toPoint()));
        if (!href.isEmpty()) {
            Q_EMIT linkActivated(href, e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ShiftModifier));
            e->accept();
            return;
        }
    }
    QPlainTextEdit::mousePressEvent(e);
}

void Editor::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    bool hand = (e->modifiers() & Qt::ControlModifier) && !linkAt(cursorForPosition(pos)).isEmpty();
    if (!hand && m_live && !(e->buttons() & Qt::LeftButton)) {
        hand = !clickedLink(pos).isEmpty();
        const QTextBlock b = cursorForPosition(pos).block();
        if (!hand && previewed(b))
            for (const Deco &d : decorations(b))
                hand = hand || (d.kind == Deco::Task && d.rect.adjusted(-3, 0, 3, 0).contains(e->position()));
    }
    viewport()->setCursor(hand ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QPlainTextEdit::mouseMoveEvent(e);
}

bool Editor::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasImage() || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
}

QString Editor::attach(const QString &absolutePath)
{
    const QFileInfo fi(absolutePath);
    if (!m_vault || !fi.isFile())
        return {};
    QString rel = m_vault->relativePath(fi.absoluteFilePath());
    if (rel.startsWith(u"../"_s) || QFileInfo(rel).isAbsolute()) {
        rel = m_vault->uniquePath(fi.fileName());
        if (!QFile::copy(fi.absoluteFilePath(), m_vault->absolutePath(rel)))
            return {};
        m_vault->rescan();
    }
    const QString link = u"[["_s + m_vault->linkText(rel) + u"]]"_s;
    return Vault::isImage(rel) ? u'!' + link : link;
}

void Editor::insertFromMimeData(const QMimeData *source)
{
    if (m_vault && source->hasImage()) {
        const QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) {
            const QString name = u"Pasted image "_s + QDateTime::currentDateTime().toString(u"yyyyMMddHHmmss"_s) + u".png"_s;
            const QString rel = m_vault->uniquePath(name);
            if (img.save(m_vault->absolutePath(rel), "PNG")) {
                m_vault->rescan();
                textCursor().insertText(u"![["_s + rel + u"]]"_s);
                return;
            }
        }
    }
    if (m_vault && source->hasUrls()) {
        QStringList links;
        for (const QUrl &url : source->urls()) {
            if (!url.isLocalFile()) {
                links.clear();
                break;
            }
            const QString l = attach(url.toLocalFile());
            if (!l.isEmpty())
                links << l;
        }
        if (!links.isEmpty()) {
            textCursor().insertText(links.join(u'\n'));
            return;
        }
    }
    if (source->hasText()) {
        textCursor().insertText(source->text());
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}

int Editor::tableIndex(const TableOverlay *overlay) const
{
    for (int i = 0; i < m_tables.size(); ++i)
        if (m_tables[i].overlay == overlay)
            return i;
    return -1;
}

int Editor::tableAt(const QTextBlock &block) const
{
    const int n = block.blockNumber();
    for (int i = 0; i < m_tables.size(); ++i) {
        const int first = m_tables[i].head.blockNumber();
        if (n >= first && n < first + m_tables[i].lines)
            return i;
    }
    return -1;
}

// Skips the delimiter line.
QTextBlock Editor::tableLine(const Table &t, int row) const
{
    return document()->findBlockByNumber(t.head.blockNumber() + (row == 0 ? 0 : row + 1));
}

void Editor::scanTables()
{
    m_tableTimer.stop();
    if (m_tableBusy)
        return;
    m_tableBusy = true;

    const auto fenced = fencedOrFrontmatter;
    // Table boundaries follow md4c so the editor agrees with the reading view.
    static const QRegularExpression heading(u"^ {0,3}#{1,6}(\\s|$)"_s);
    static const QRegularExpression rule(u"^ {0,3}([-*_])( *\\1){2,} *$"_s);
    static const QRegularExpression ender(u"^ {0,3}([-*+][ \\t]|\\d+[.)][ \\t]|>)"_s);
    auto opens = [&](const QTextBlock &prev) {
        if (!prev.isValid() || prev.text().trimmed().isEmpty())
            return true;
        const int st = prev.userState();
        const bool fenceClose = st > 0 && (st & BlockState::FenceLine) && !(st & BlockState::InFence);
        return fenceClose || heading.match(prev.text()).hasMatch() || rule.match(prev.text()).hasMatch();
    };
    auto continues = [&](const QTextBlock &b) {
        const QString t = b.text();
        return !t.trimmed().isEmpty() && !fenced(b) && !ender.match(t).hasMatch() && !rule.match(t).hasMatch();
    };
    const int cursorBlock = hasFocus() ? textCursor().blockNumber() : -1;
    auto inRange = [](int n, const QTextBlock &first, int lines) {
        return n >= first.blockNumber() && n < first.blockNumber() + lines;
    };
    QList<QPair<QTextBlock, int>> found;
    bool rawFound = false;
    for (QTextBlock b = m_live ? document()->begin() : QTextBlock(); b.isValid();) {
        const QTextBlock next = b.next();
        if (!fenced(b) && next.isValid() && opens(b.previous()) && MdTable::hasPipe(b.text())
            && MdTable::isDelimiter(next.text())
            && MdTable::splitRow(b.text()).size() == MdTable::splitRow(next.text()).size()) {
            int lines = 2;
            QTextBlock e = next.next();
            while (e.isValid() && continues(e)) {
                ++lines;
                e = e.next();
            }
            const bool typing = inRange(cursorBlock, b, lines) && inRange(m_typedBlock, b, lines);
            const bool chosen = m_rawTable.isValid() && b == m_rawTable && inRange(textCursor().blockNumber(), b, lines);
            if (chosen || typing) {
                m_rawTable = b;
                m_rawLines = lines;
                rawFound = true;
            } else {
                found.append({b, lines});
            }
            b = e;
            continue;
        }
        b = next;
    }
    if (!rawFound)
        m_rawTable = QTextBlock();

    while (m_tables.size() > found.size()) {
        TableOverlay *o = m_tables.takeLast().overlay;
        o->hide();
        o->deleteLater();
    }
    QSet<int> covered;
    for (int i = 0; i < found.size(); ++i) {
        if (i == m_tables.size()) {
            auto *o = new TableOverlay(viewport());
            o->setNoteFont(font());
            QPointer<TableOverlay> guard(o);
            connect(o, &TableOverlay::changed, this,
                    [this, guard](const QList<QStringList> &rows, const QStringList &delims, int r, int c, bool edit) {
                        // Leave the grid's own signal handler before the overlays get rebuilt.
                        m_pendingWrites.append({guard, rows, delims, r, c, edit});
                        QTimer::singleShot(0, this, &Editor::flushTableWrites);
                    });
            connect(o, &TableOverlay::leave, this, [this, o](bool down) { leaveTable(o, down); });
            connect(o, &TableOverlay::editSource, this, [this, o] { editTableSource(o); });
            connect(o, &TableOverlay::deleteTable, this, [this, o] { deleteTable(o); });
            connect(o, &TableOverlay::undo, this, &QPlainTextEdit::undo);
            connect(o, &TableOverlay::redo, this, &QPlainTextEdit::redo);
            connect(o, &TableOverlay::rowFocused, this, [this, o](int row) { showTableRow(o, row); });
            connect(o, &TableOverlay::linkActivated, this, &Editor::linkActivated);
            o->setCellRenderer([this](const QString &cell) {
                if (!m_vault)
                    return cell.toHtmlEscaped();
                RenderOptions opt;
                opt.vault = m_vault;
                opt.notePath = m_notePath;
                opt.contentWidth = 240;
                opt.palette = palette();
                opt.dark = palette().color(QPalette::Base).lightness() < 128;
                opt.depth = 2; // no task toggles or nested note embeds inside a cell
                return renderTableCell(cell, opt);
            });
            m_tables.append({{}, 0, 0, o});
        }
        Table &t = m_tables[i];
        t.head = found[i].first;
        t.lines = found[i].second;
        QList<QStringList> rows;
        QStringList delims;
        QTextBlock b = t.head;
        for (int k = 0; k < t.lines; ++k, b = b.next()) {
            covered.insert(b.blockNumber());
            if (k == 1)
                delims = MdTable::splitRow(b.text());
            else
                rows << MdTable::splitRow(b.text());
        }
        t.overlay->setTable(rows, delims);
    }

    scanProperties(covered);
    scanImages(covered);

    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        if (covered.contains(b.blockNumber()))
            continue;
        if (!b.isVisible()) {
            b.setVisible(true);
            document()->markContentsDirty(b.position(), b.length());
        }
        if (auto *d = static_cast<TableLineData *>(b.userData()); d && d->pixelSize) {
            d->pixelSize = 0;
            m_highlighter->rehighlightBlock(b);
        }
    }
    m_tableBusy = false;
    layoutTables();
    updateTableHint();
    viewport()->update();
}

void Editor::setLineHeight(const QTextBlock &block, int height)
{
    QTextBlock b = block;
    auto *d = static_cast<TableLineData *>(b.userData());
    if (!d) {
        d = new TableLineData;
        b.setUserData(d);
    }
    QFont f = font();
    f.setPixelSize(100);
    const double perPixel = QFontMetricsF(f).height() / 100.0;
    int px = d->pixelSize ? d->pixelSize : qMax(1, int(height / perPixel));
    // The text layout decides the real line height, so measure and adjust.
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (d->pixelSize != px) {
            d->pixelSize = px;
            m_highlighter->rehighlightBlock(b);
        }
        const double actual = document()->documentLayout()->blockBoundingRect(b).height();
        if (actual >= height && actual - height <= 2 * perPixel + 1)
            break;
        int step = int((height - actual) / perPixel);
        if (step == 0)
            step = actual < height ? 1 : -1;
        px = qMax(1, px + step);
    }
}

void Editor::layoutTables()
{
    if (m_tableBusy || (m_tables.isEmpty() && m_images.isEmpty() && !m_propLines))
        return;
    m_tableBusy = true;
    const int margin = int(document()->documentMargin());
    const int width = viewport()->width() - 2 * margin;
    auto *layout = document()->documentLayout();
    if (m_propLines) {
        for (QTextBlock b = document()->firstBlock().next(); b.isValid() && b.blockNumber() < m_propLines; b = b.next()) {
            if (b.isVisible()) {
                b.setVisible(false);
                document()->markContentsDirty(b.position(), b.length());
            }
        }
        setLineHeight(document()->firstBlock(), m_props->heightFor(width));
        m_propsHeight = int(layout->blockBoundingRect(document()->firstBlock()).height());
    }
    for (Image &im : m_images) {
        setLineHeight(im.block, im.widget->sizeFor(width).height() + 8);
        im.height = int(layout->blockBoundingRect(im.block).height());
    }
    for (Table &t : m_tables) {
        const QList<int> want = t.overlay->layoutFor(width);
        const QTextBlock delimiter = t.head.next();
        if (QTextBlock d = delimiter; d.isVisible()) {
            d.setVisible(false);
            document()->markContentsDirty(delimiter.position(), delimiter.length());
        }
        QList<int> actual;
        t.height = 0;
        for (int row = 0; row < want.size(); ++row) {
            const QTextBlock b = tableLine(t, row);
            setLineHeight(b, want[row]);
            actual << int(layout->blockBoundingRect(b).height());
            t.height += actual.last();
        }
        t.overlay->setLineHeights(actual);
    }
    m_tableBusy = false;
    positionTables();
}

void Editor::positionTables()
{
    if (m_tableTimer.isActive())
        return; // block handles may be stale until the rescan
    const QPointF offset = contentOffset();
    const int margin = int(document()->documentMargin());
    const int width = viewport()->width() - 2 * margin;
    for (const Table &t : std::as_const(m_tables)) {
        const QRectF r = blockBoundingGeometry(t.head).translated(offset);
        t.overlay->setGeometry(margin, int(r.top()), width, t.height);
        t.overlay->show();
    }
    for (const Image &im : std::as_const(m_images)) {
        const QRectF r = blockBoundingGeometry(im.block).translated(offset);
        const QSize size = im.widget->sizeFor(width);
        im.widget->setGeometry(margin, int(r.top()) + (im.height - size.height()) / 2, size.width(), size.height());
        im.widget->show();
    }
    if (m_props && m_propLines) {
        const QRectF r = blockBoundingGeometry(document()->firstBlock()).translated(offset);
        m_props->setGeometry(margin, int(r.top()), width, m_propsHeight);
        m_props->show();
    } else if (m_props) {
        m_props->hide();
    }
}

void Editor::moveCursorQuietly(const QTextCursor &cursor)
{
    const bool busy = m_tableBusy;
    m_tableBusy = true;
    setTextCursor(cursor);
    m_tableBusy = busy;
}

void Editor::onCursorMoved()
{
    if (m_tableBusy || m_tableTimer.isActive())
        return;
    const QTextCursor c = textCursor();
    const int n = c.block().blockNumber();
    bool rescan = false;
    if (m_rawImage.isValid() && c.block() != m_rawImage) {
        m_rawImage = QTextBlock();
        rescan = true;
    }
    if (m_rawProps && n > frontmatterEnd()) {
        m_rawProps = false;
        m_typedBlock = -1;
        rescan = true;
    }
    if (n < m_propLines) {
        if (m_navKey && !c.hasSelection()) {
            m_props->focusRow(m_navUp);
        } else {
            m_rawProps = true;
            scanTables();
            ensureCursorVisible();
        }
        return;
    }
    for (const Image &im : std::as_const(m_images)) {
        if (im.block == c.block()) {
            m_rawImage = im.block;
            scanTables();
            ensureCursorVisible();
            return;
        }
    }
    if (rescan)
        m_tableTimer.start();
    if (m_rawTable.isValid()) {
        const int first = m_rawTable.blockNumber();
        if (n < first || n >= first + m_rawLines) {
            m_rawTable = QTextBlock();
            m_typedBlock = -1;
            m_tableTimer.start();
            return;
        }
    }
    const int i = tableAt(c.block());
    if (i < 0)
        return;
    if (m_navKey) {
        if (!c.hasSelection())
            m_tables[i].overlay->focusCell(m_navUp ? m_tables[i].overlay->lineCount() - 1 : 0, 0, false);
        return;
    }
    // Find, go-to-line or a selection landed inside the grid: show the source.
    m_rawTable = m_tables[i].head;
    m_rawLines = m_tables[i].lines;
    scanTables();
    ensureCursorVisible();
}

void Editor::leaveTable(TableOverlay *overlay, bool down)
{
    const int i = tableIndex(overlay);
    if (i < 0)
        return;
    const Table t = m_tables[i];
    // Keep a blank line between table and text, or Markdown merges them.
    auto blank = [](const QTextBlock &b) { return b.text().trimmed().isEmpty(); };
    auto addLines = [this](bool atEnd, const QString &text) {
        QTextCursor c(document());
        c.movePosition(atEnd ? QTextCursor::End : QTextCursor::Start);
        c.insertText(text);
        scanTables();
    };
    QTextBlock target;
    if (down) {
        const QTextBlock next = document()->findBlockByNumber(t.head.blockNumber() + t.lines);
        if (!next.isValid())
            addLines(true, u"\n\n"_s);
        else if (blank(next) && !next.next().isValid())
            addLines(true, u"\n"_s);
        else
            target = blank(next) ? next.next() : next;
        if (!target.isValid())
            target = document()->lastBlock();
    } else {
        const QTextBlock prev = document()->findBlockByNumber(t.head.blockNumber() - 1);
        if (!prev.isValid())
            addLines(false, u"\n\n"_s);
        else if (blank(prev) && !prev.previous().isValid())
            addLines(false, u"\n"_s);
        else
            target = blank(prev) ? prev.previous() : prev;
        if (!target.isValid())
            target = document()->firstBlock();
    }
    QTextCursor c(target);
    if (!down)
        c.movePosition(QTextCursor::EndOfBlock);
    moveCursorQuietly(c);
    setFocus();
    ensureCursorVisible();
}

void Editor::editTableSource(TableOverlay *overlay)
{
    const int i = tableIndex(overlay);
    if (i < 0)
        return;
    m_rawTable = m_tables[i].head;
    m_rawLines = m_tables[i].lines;
    const QTextCursor c(m_rawTable);
    scanTables();
    moveCursorQuietly(c);
    setFocus();
    ensureCursorVisible();
}

void Editor::deleteTable(TableOverlay *overlay)
{
    const int i = tableIndex(overlay);
    if (i < 0)
        return;
    const Table t = m_tables[i];
    const QTextBlock last = document()->findBlockByNumber(t.head.blockNumber() + t.lines - 1);
    int start = t.head.position();
    int end = last.position() + last.length() - 1;
    if (last.next().isValid())
        ++end;
    else if (start > 0)
        --start;
    QTextCursor c(document());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    scanTables();
    moveCursorQuietly(c);
    setFocus();
}

void Editor::writeTable(TableOverlay *overlay, const QList<QStringList> &rows, const QStringList &delims,
                        int focusRow, int focusCol, bool edit)
{
    const int i = tableIndex(overlay);
    if (i < 0 || rows.isEmpty())
        return;
    const Table t = m_tables[i];
    const qsizetype oldCols = overlay->delims().size();

    QList<QTextBlock> blocks;
    QStringList oldLines;
    QList<QStringList> before, after;
    QTextBlock b = t.head;
    for (int k = 0; k < t.lines; ++k, b = b.next()) {
        blocks << b;
        oldLines << b.text();
        QStringList cells = MdTable::splitRow(b.text());
        if (k != 1)
            cells.resize(oldCols);
        before << cells;
    }
    after << rows.first() << delims << rows.mid(1);

    // Untouched rows keep their exact text; padded tables stay padded.
    qsizetype same = 0;
    while (same < before.size() && same < after.size() && before[same] == after[same])
        ++same;
    qsizetype sameEnd = 0;
    while (sameEnd < before.size() - same && sameEnd < after.size() - same
           && before[before.size() - 1 - sameEnd] == after[after.size() - 1 - sameEnd])
        ++sameEnd;
    QStringList newLines;
    for (qsizetype k = 0; k < after.size(); ++k) {
        if (k < same)
            newLines << oldLines[k];
        else if (k >= after.size() - sameEnd)
            newLines << oldLines[k - after.size() + before.size()];
        else
            newLines << (k == 1 ? u"| "_s + after[1].join(u" | "_s) + u" |"_s : MdTable::joinRow(after[k]));
    }
    if (MdTable::isAligned(oldLines))
        newLines = MdTable::alignedLines(after);

    qsizetype p = 0;
    while (p < oldLines.size() && p < newLines.size() && oldLines[p] == newLines[p])
        ++p;
    qsizetype s = 0;
    while (s < oldLines.size() - p && s < newLines.size() - p
           && oldLines[oldLines.size() - 1 - s] == newLines[newLines.size() - 1 - s])
        ++s;
    const qsizetype oldMid = oldLines.size() - p - s;
    const qsizetype newMid = newLines.size() - p - s;

    if (oldMid || newMid) {
        m_tableBusy = true;
        QTextCursor c(document());
        c.beginEditBlock();
        if (oldMid == newMid) {
            for (qsizetype k = p; k < p + oldMid; ++k) {
                if (oldLines[k] == newLines[k])
                    continue;
                c.setPosition(blocks[k].position());
                c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                c.insertText(newLines[k]);
            }
        } else {
            const QStringList text = newLines.mid(p, newMid);
            if (oldMid == 0) {
                const QTextBlock prev = blocks[p - 1];
                c.setPosition(prev.position() + prev.length() - 1);
                c.insertText(u'\n' + text.join(u'\n'));
            } else {
                const QTextBlock last = blocks[p + oldMid - 1];
                int start = blocks[p].position();
                if (newMid == 0)
                    --start; // the newline before the removed rows
                c.setPosition(start);
                c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
                c.insertText(text.join(u'\n'));
            }
        }
        c.endEditBlock();
        m_tableBusy = false;
        scanTables();
    }
    if (i < m_tables.size())
        m_tables[i].overlay->focusCell(focusRow, focusCol, edit);
}

void Editor::showTableRow(TableOverlay *overlay, int row)
{
    const int i = tableIndex(overlay);
    if (i < 0)
        return;
    const QTextBlock b = tableLine(m_tables[i], row);
    QScrollBar *bar = verticalScrollBar();
    QRectF r = blockBoundingGeometry(b).translated(contentOffset());
    if (r.top() < 0) {
        bar->setValue(b.firstLineNumber());
        return;
    }
    for (int guard = 0; guard < 500 && r.bottom() > viewport()->height() && bar->value() < bar->maximum(); ++guard) {
        bar->setValue(bar->value() + 1);
        r = blockBoundingGeometry(b).translated(contentOffset());
    }
}

void Editor::flushTableWrites()
{
    const QList<PendingWrite> writes = std::exchange(m_pendingWrites, {});
    for (const PendingWrite &w : writes)
        if (w.overlay)
            writeTable(w.overlay, w.rows, w.delims, w.row, w.col, w.edit);
}

void Editor::commitTables()
{
    for (const Table &t : std::as_const(m_tables))
        t.overlay->finishEditing();
    flushTableWrites();
}

void Editor::setCommands(std::function<QAction *(const QString &)> command,
                         std::function<void(QMenu *, const QString &, bool)> fileActions)
{
    m_command = std::move(command);
    m_fileActions = std::move(fileActions);
}

// Window shortcuts beat the text edit's own keys (in Qt, Ctrl+K deletes to the end of the line).
bool Editor::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        const QKeySequence key(static_cast<QKeyEvent *>(e)->keyCombination());
        for (const QAction *a : window()->actions()) {
            if (a->isEnabled() && a->shortcuts().contains(key)) {
                e->ignore();
                return true;
            }
        }
    }
    return QPlainTextEdit::event(e);
}

void Editor::contextMenuEvent(QContextMenuEvent *e)
{
    if (!m_command) {
        QPlainTextEdit::contextMenuEvent(e);
        return;
    }
    QMenu menu(this);
    auto add = [&](QMenu *to, const QString &id) {
        if (QAction *a = m_command(id))
            to->addAction(a);
    };
    const QString href = linkAt(cursorForPosition(e->pos()));
    if (!href.isEmpty()) {
        menu.addAction(tr("Open in new tab"), this, [this, href] { Q_EMIT linkActivated(href, true); });
        menu.addSeparator();
    }
    add(&menu, u"link.internal"_s);
    add(&menu, u"link.external"_s);
    menu.addSeparator();
    if (!href.isEmpty()) {
        menu.addAction(tr("Edit link"), this, [this, pos = e->pos()] {
            // Select the target so typing replaces it.
            QTextCursor c = cursorForPosition(pos);
            static const QRegularExpression link(u"\\[\\[([^\\[\\]|#\\n]+)|\\]\\(([^)\\s]+)\\)"_s);
            const QString line = c.block().text();
            for (auto it = link.globalMatch(line); it.hasNext();) {
                const auto m = it.next();
                const int group = m.capturedLength(1) ? 1 : 2;
                if (c.positionInBlock() >= m.capturedStart() - 2 && c.positionInBlock() <= m.capturedEnd() + 2) {
                    c.setPosition(c.block().position() + int(m.capturedStart(group)));
                    c.setPosition(c.block().position() + int(m.capturedEnd(group)), QTextCursor::KeepAnchor);
                    setTextCursor(c);
                    break;
                }
            }
        });
    } else {
        QMenu *format = menu.addMenu(tr("Format"));
        for (const char16_t *id : {u"format.bold", u"format.italic", u"format.strike", u"format.highlight"})
            add(format, QString::fromUtf16(id));
        format->addSeparator();
        add(format, u"format.code"_s);
        add(format, u"format.comment"_s);
        format->addSeparator();
        add(format, u"format.clear"_s);
    }
    QMenu *paragraph = menu.addMenu(tr("Paragraph"));
    for (const char16_t *id : {u"para.bullet", u"para.numbered", u"para.task"})
        add(paragraph, QString::fromUtf16(id));
    paragraph->addSeparator();
    for (int level = 1; level <= 6; ++level)
        add(paragraph, u"para.h%1"_s.arg(level));
    add(paragraph, u"para.body"_s);
    paragraph->addSeparator();
    add(paragraph, u"para.quote"_s);
    QMenu *insert = menu.addMenu(tr("Insert"));
    for (const char16_t *id : {u"insert.table", u"insert.callout", u"insert.rule"})
        add(insert, QString::fromUtf16(id));
    insert->addSeparator();
    add(insert, u"insert.codeblock"_s);
    menu.addSeparator();

    const bool selection = textCursor().hasSelection();
    menu.addAction(tr("Cut"), QKeySequence::Cut, this, &QPlainTextEdit::cut)->setEnabled(selection && !isReadOnly());
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, &QPlainTextEdit::copy)->setEnabled(selection);
    menu.addAction(tr("Paste"), QKeySequence::Paste, this, &QPlainTextEdit::paste)->setEnabled(canPaste());
    add(&menu, u"edit.pastePlain"_s);
    menu.addAction(tr("Select all"), QKeySequence::SelectAll, this, &QPlainTextEdit::selectAll);

    if (href.startsWith(u"wiki:"_s) && m_vault && m_fileActions) {
        const QString target = QUrl::fromPercentEncoding(href.mid(5).toUtf8()).section(u'#', 0, 0);
        if (const QString path = m_vault->resolve(target, m_notePath); !path.isEmpty()) {
            menu.addSeparator();
            m_fileActions(&menu, path, false);
        }
    }
    menu.exec(e->globalPos());
}

// Wraps the selection (or the word at the cursor) in `mark`, or unwraps it if it is already wrapped.
void Editor::toggleWrap(const QString &mark)
{
    QTextCursor c = textCursor();
    if (!c.hasSelection())
        c.select(QTextCursor::WordUnderCursor);
    if (!c.hasSelection()) {
        c.insertText(mark + mark);
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, int(mark.size()));
        setTextCursor(c);
        return;
    }
    const int n = int(mark.size());
    auto textAt = [this](int pos, int len) {
        if (pos < 0)
            return QString();
        QTextCursor t(document());
        t.setPosition(pos);
        t.setPosition(qMin(pos + len, document()->characterCount() - 1), QTextCursor::KeepAnchor);
        return t.selectedText();
    };
    int start = c.selectionStart(), end = c.selectionEnd();
    const QString inner = c.selectedText();
    c.beginEditBlock();
    if (textAt(start - n, n) == mark && textAt(end, n) == mark) {
        c.setPosition(end);
        c.setPosition(end + n, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(start - n);
        c.setPosition(start, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        start -= n;
        end -= n;
    } else if (inner.size() >= 2 * n && inner.startsWith(mark) && inner.endsWith(mark)) {
        c.insertText(inner.mid(n, inner.size() - 2 * n));
        end -= 2 * n;
    } else {
        c.setPosition(end);
        c.insertText(mark);
        c.setPosition(start);
        c.insertText(mark);
        start += n;
        end += n;
    }
    c.endEditBlock();
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    setTextCursor(c);
}

// Puts `text` on its own lines with blank lines around it, so Markdown reads it as a block.
void Editor::insertBlock(const QString &text, int cursorAt)
{
    auto blank = [](const QTextBlock &b) { return b.text().trimmed().isEmpty(); };
    QTextCursor c = textCursor();
    c.clearSelection();
    c.movePosition(QTextCursor::EndOfBlock);
    QString before;
    if (!blank(c.block()))
        before = u"\n\n"_s;
    else if (c.block().previous().isValid() && !blank(c.block().previous()))
        before = u"\n"_s;
    const QTextBlock next = c.block().next();
    const QString after = !next.isValid() || !blank(next) ? u"\n"_s : QString();
    c.beginEditBlock();
    c.insertText(before + text + after);
    c.endEditBlock();

    const int start = c.position() - int(after.size()) - int(text.size());
    if (cursorAt >= 0) {
        c.setPosition(start + cursorAt);
        setTextCursor(c);
        return;
    }
    const QTextBlock first = document()->findBlock(start);
    const QTextBlock below = document()->findBlockByNumber(first.blockNumber() + int(text.count(u'\n')) + 1);
    moveCursorQuietly(QTextCursor(below.isValid() ? below : document()->lastBlock()));
}

void Editor::insertTable()
{
    const int at = textCursor().position();
    insertBlock(u"|     |     |\n| --- | --- |\n|     |     |"_s);
    scanTables();
    for (const Table &t : std::as_const(m_tables))
        if (t.head.position() >= at)
            return t.overlay->focusCell(0, 0, true);
}

void Editor::insertCallout()
{
    const QString text = u"> [!note]\n> "_s;
    insertBlock(text, int(text.size()));
}

void Editor::insertCodeBlock()
{
    insertBlock(u"```\n\n```"_s, 4);
}

void Editor::insertRule()
{
    insertBlock(u"---"_s);
}

void Editor::addInternalLink()
{
    QTextCursor c = textCursor();
    if (c.hasSelection()) {
        c.insertText(u"[["_s + c.selectedText() + u"]]"_s);
        return;
    }
    c.insertText(u"[[]]"_s);
    c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
    setTextCursor(c);
    setFocus();
    updateCompletion();
}

void Editor::insertMarkdownLink()
{
    QTextCursor c = textCursor();
    const QString selected = c.selectedText();
    const bool url = QUrl(selected, QUrl::StrictMode).isValid() && selected.contains(u"://"_s);
    if (url) {
        c.insertText(u"[](%1)"_s.arg(selected));
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, int(selected.size()) + 3);
    } else {
        c.insertText(u"[%1]()"_s.arg(selected));
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, selected.isEmpty() ? 3 : 1);
    }
    setTextCursor(c);
}

void Editor::followLinkAtCursor(bool newTab)
{
    if (const QString href = linkAt(textCursor()); !href.isEmpty())
        Q_EMIT linkActivated(href, newTab);
}

void Editor::pasteAsPlainText()
{
    textCursor().insertText(QApplication::clipboard()->text());
}

void Editor::clearFormatting()
{
    QTextCursor c = textCursor();
    if (!c.hasSelection())
        c.select(QTextCursor::BlockUnderCursor);
    QString text = c.selectedText();
    static const QRegularExpression marks[] = {
        QRegularExpression(u"\\*\\*(.+?)\\*\\*"_s), QRegularExpression(u"__(.+?)__"_s), QRegularExpression(u"~~(.+?)~~"_s),
        QRegularExpression(u"==(.+?)=="_s), QRegularExpression(u"%%(.+?)%%"_s), QRegularExpression(u"`([^`]+)`"_s),
        QRegularExpression(u"(?<![*\\w])\\*(?!\\s)(.+?)(?<!\\s)\\*(?![*\\w])"_s), QRegularExpression(u"(?<![_\\w])_(?!\\s)(.+?)(?<!\\s)_(?![_\\w])"_s)};
    for (const QRegularExpression &re : marks)
        text.replace(re, u"\\1"_s);
    c.insertText(text);
}

void Editor::setParagraph(Paragraph kind, int level)
{
    static const QRegularExpression prefix(u"^(\\s*)(#{1,6}\\s+|>\\s?|[-*+]\\s+\\[.\\]\\s+|[-*+]\\s+|\\d+[.)]\\s+)?"_s);
    QTextCursor c = textCursor();
    QTextBlock b = document()->findBlock(c.selectionStart());
    const QTextBlock last = document()->findBlock(c.selectionEnd());
    int number = 1;
    c.beginEditBlock();
    for (;; b = b.next()) {
        const auto m = prefix.match(b.text());
        const QString existing = m.captured(2).trimmed();
        QString wanted;
        switch (kind) {
        case Paragraph::Bullet: wanted = u"- "_s; break;
        case Paragraph::Numbered: wanted = QString::number(number++) + u". "_s; break;
        case Paragraph::Task: wanted = u"- [ ] "_s; break;
        case Paragraph::Heading: wanted = QString(level, u'#') + u' '; break;
        case Paragraph::Quote: wanted = u"> "_s; break;
        case Paragraph::Body: break;
        }
        // Applying the kind a line already has turns it back into plain text.
        const bool same = (kind == Paragraph::Numbered && !existing.isEmpty() && existing[0].isDigit())
                          || (kind == Paragraph::Task && existing.endsWith(u']'))
                          || (kind == Paragraph::Bullet && (existing == u"-" || existing == u"*" || existing == u"+"))
                          || (kind != Paragraph::Numbered && existing == wanted.trimmed() && !wanted.isEmpty());
        if (same)
            wanted.clear();
        const bool keepIndent = kind != Paragraph::Heading;
        QTextCursor line(b);
        line.setPosition(b.position() + (keepIndent ? int(m.capturedLength(1)) : 0));
        line.setPosition(b.position() + int(m.capturedEnd(0)), QTextCursor::KeepAnchor);
        line.insertText(wanted);
        if (b == last)
            break;
    }
    c.endEditBlock();
}

// Ctrl+L: a line becomes a task, a task's box is ticked or cleared.
void Editor::toggleCheckbox()
{
    static const QRegularExpression item(u"^(\\s*)([-*+]|\\d+[.)])\\s+(\\[(.)\\]\\s)?"_s);
    QTextCursor c = textCursor();
    QTextBlock b = document()->findBlock(c.selectionStart());
    const QTextBlock last = document()->findBlock(c.selectionEnd());
    c.beginEditBlock();
    for (;; b = b.next()) {
        const auto m = item.match(b.text());
        QTextCursor t(b);
        if (m.hasMatch() && m.capturedLength(3)) {
            t.setPosition(b.position() + int(m.capturedStart(4)));
            t.setPosition(b.position() + int(m.capturedEnd(4)), QTextCursor::KeepAnchor);
            t.insertText(m.captured(4) == u" " ? u"x"_s : u" "_s);
        } else if (m.hasMatch()) {
            t.setPosition(b.position() + int(m.capturedEnd(0)));
            t.insertText(u"[ ] "_s);
        } else {
            int at = 0;
            while (at < b.text().size() && b.text()[at].isSpace())
                ++at;
            t.setPosition(b.position() + at);
            t.insertText(u"- [ ] "_s);
        }
        if (b == last)
            break;
    }
    c.endEditBlock();
}

void Editor::deleteParagraph()
{
    QTextCursor c = textCursor();
    const QTextBlock first = document()->findBlock(c.selectionStart());
    const QTextBlock last = document()->findBlock(c.selectionEnd());
    int start = first.position();
    int end = last.position() + last.length() - 1;
    if (last.next().isValid())
        ++end;
    else if (start > 0)
        --start;
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
}

// Ctrl+; adds a row to the properties panel, or in source mode a frontmatter line with the name selected.
void Editor::addProperty()
{
    if (m_live) {
        if (frontmatterEnd() < 0) {
            m_tableBusy = true;
            QTextCursor(document()).insertText(u"---\n---\n"_s);
            m_tableBusy = false;
        }
        m_rawProps = false;
        scanTables();
        if (m_propLines) {
            m_props->addProperty();
            return;
        }
    }
    const QString name = tr("property");
    QTextCursor c(document());
    int lineStart = -1;
    if (document()->firstBlock().text() == u"---") {
        for (QTextBlock b = document()->firstBlock().next(); b.isValid(); b = b.next()) {
            if (b.text() == u"---" || b.text() == u"...") {
                c.setPosition(b.position());
                c.insertText(name + u": \n"_s);
                lineStart = b.previous().position();
                break;
            }
        }
    }
    if (lineStart < 0) {
        c.setPosition(0);
        c.insertText(u"---\n"_s + name + u": \n---\n"_s);
        lineStart = 4;
    }
    c.setPosition(lineStart);
    c.setPosition(lineStart + int(name.size()), QTextCursor::KeepAnchor);
    setTextCursor(c);
    setFocus();
}

// Block number of the closing "---", or -1 when the note has no frontmatter.
int Editor::frontmatterEnd() const
{
    const QTextBlock first = document()->firstBlock();
    if (first.text() != u"---")
        return -1;
    for (QTextBlock b = first.next(); b.isValid(); b = b.next())
        if (b.text() == u"---" || b.text() == u"...")
            return b.blockNumber();
    return -1;
}

void Editor::scanProperties(QSet<int> &covered)
{
    const int end = m_live ? frontmatterEnd() : -1;
    const int cursor = textCursor().blockNumber();
    if (end > 0 && cursor <= end && m_typedBlock >= 0 && m_typedBlock <= end)
        m_rawProps = true; // the frontmatter is being typed as text
    if (end < 0 || m_rawProps) {
        m_propLines = 0;
        if (m_props)
            m_props->hide();
        return;
    }
    if (!m_props) {
        m_props = new PropertiesPanel(viewport());
        connect(m_props, &PropertiesPanel::changed, this, [this](const QStringList &lines) {
            // Leave the panel's signal handler before the document changes under it.
            QTimer::singleShot(0, this, [this, lines] { writeFrontmatter(lines); });
        });
        connect(m_props, &PropertiesPanel::leave, this, &Editor::leaveProperties);
        connect(m_props, &PropertiesPanel::editSource, this, &Editor::showPropertiesSource);
        connect(m_props, &PropertiesPanel::heightChanged, this, &Editor::layoutTables);
    }
    m_propLines = end + 1;
    QStringList lines;
    for (QTextBlock b = document()->firstBlock(); b.isValid() && b.blockNumber() <= end; b = b.next()) {
        covered.insert(b.blockNumber());
        if (b.blockNumber() > 0 && b.blockNumber() < end)
            lines << b.text();
    }
    m_props->setSource(lines);
    if (cursor <= end && !m_props->isAncestorOf(QApplication::focusWidget())) {
        const QTextBlock body = document()->findBlockByNumber(end + 1);
        QTextCursor c(document());
        c.setPosition(body.isValid() ? body.position() : document()->lastBlock().position()
                                                             + document()->lastBlock().length() - 1);
        moveCursorQuietly(c);
    }
}

void Editor::writeFrontmatter(const QStringList &lines)
{
    const int end = frontmatterEnd();
    if (end < 0)
        return;
    QStringList old;
    for (QTextBlock b = document()->firstBlock().next(); b.blockNumber() < end; b = b.next())
        old << b.text();
    if (old == lines && !lines.isEmpty())
        return;
    m_tableBusy = true;
    QTextCursor c(document());
    c.beginEditBlock();
    if (lines.isEmpty()) {
        const QTextBlock close = document()->findBlockByNumber(end);
        c.setPosition(0);
        c.setPosition(qMin(close.position() + close.length(), document()->characterCount() - 1), QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else {
        // Only the lines that differ are replaced, so undo stays small.
        qsizetype p = 0;
        while (p < old.size() && p < lines.size() && old[p] == lines[p])
            ++p;
        qsizetype s = 0;
        while (s < old.size() - p && s < lines.size() - p && old[old.size() - 1 - s] == lines[lines.size() - 1 - s])
            ++s;
        const QTextBlock from = document()->findBlockByNumber(int(1 + p));
        const QTextBlock to = document()->findBlockByNumber(int(1 + old.size() - s));
        c.setPosition(from.position());
        c.setPosition(to.position(), QTextCursor::KeepAnchor);
        const QStringList mid = lines.mid(p, lines.size() - p - s);
        c.insertText(mid.isEmpty() ? QString() : mid.join(u'\n') + u'\n');
    }
    c.endEditBlock();
    m_tableBusy = false;
    scanTables();
}

void Editor::leaveProperties()
{
    QTextBlock body = document()->findBlockByNumber(m_propLines);
    if (!body.isValid()) {
        QTextCursor c(document());
        c.movePosition(QTextCursor::End);
        m_tableBusy = true;
        c.insertText(u"\n"_s);
        m_tableBusy = false;
        body = document()->lastBlock();
    }
    moveCursorQuietly(QTextCursor(body));
    setFocus();
    ensureCursorVisible();
}

void Editor::showPropertiesSource()
{
    m_rawProps = true;
    scanTables();
    moveCursorQuietly(QTextCursor(document()->firstBlock().next()));
    setFocus();
    ensureCursorVisible();
}

void Editor::scanImages(QSet<int> &covered)
{
    static const QRegularExpression wiki(u"^\\s*!\\[\\[([^\\[\\]|#\\n]+)(?:#[^\\[\\]|\\n]*)?(?:\\|([^\\[\\]\\n]*))?\\]\\]\\s*$"_s);
    static const QRegularExpression md(u"^\\s*!\\[([^\\]\\n]*)\\]\\(<?([^)<>\\n]+?)>?(?:\\s+\"[^\"]*\")?\\)\\s*$"_s);
    static const QRegularExpression size(u"^\\s*(\\d+)(?:x(\\d+))?\\s*$"_s);
    QList<Image> old = std::exchange(m_images, {});
    for (QTextBlock b = m_live && m_vault ? document()->begin() : QTextBlock(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        if (!text.contains(u"!["_s) || covered.contains(b.blockNumber()) || fencedOrFrontmatter(b) || b == m_rawImage)
            continue;
        QString path, spec;
        if (const auto m = wiki.match(text); m.hasMatch()) {
            path = m_vault->resolve(m.captured(1).trimmed(), m_notePath);
            spec = m.captured(2);
        } else if (const auto m = md.match(text); m.hasMatch()) {
            const QString src = m.captured(2).trimmed();
            const QUrl url(src);
            if (!url.scheme().isEmpty() && !url.isLocalFile())
                continue;
            const QString target = url.isLocalFile() ? url.toLocalFile() : QUrl::fromPercentEncoding(src.toUtf8());
            path = m_vault->resolve(target, m_notePath);
            if (path.isEmpty()) {
                const QString abs = QFileInfo(m_vault->absolutePath(m_notePath)).dir().absoluteFilePath(target);
                if (QFileInfo(abs).isFile())
                    path = m_vault->relativePath(QDir::cleanPath(abs));
            }
            spec = m.captured(1).section(u'|', -1);
        }
        if (path.isEmpty() || !Vault::isImage(path))
            continue;
        const QString abs = m_vault->absolutePath(path);
        if (!QFileInfo(abs).isFile())
            continue;
        ImageBlock *w = nullptr;
        for (int i = 0; i < old.size() && !w; ++i)
            if (old[i].widget->path == path)
                w = old.takeAt(i).widget;
        if (!w && !old.isEmpty())
            w = old.takeFirst().widget;
        if (!w) {
            w = new ImageBlock(viewport());
            w->menu = [this, w](const QPoint &pos) { showImageMenu(w->path, pos); };
            w->clicked = [this, w] {
                for (const Image &im : std::as_const(m_images)) {
                    if (im.widget == w) {
                        QTextCursor c(im.block);
                        c.movePosition(QTextCursor::EndOfBlock);
                        setTextCursor(c);
                        setFocus();
                        return;
                    }
                }
            };
        }
        const auto sm = size.match(spec);
        w->path = path;
        w->setImage(abs, sm.hasMatch() ? QSize(sm.captured(1).toInt(), sm.captured(2).toInt()) : QSize());
        if (w->isNull()) {
            old << Image{{}, 0, w};
            continue;
        }
        m_images.append({b, 0, w});
        covered.insert(b.blockNumber());
    }
    for (const Image &im : std::as_const(old)) {
        im.widget->hide();
        im.widget->deleteLater();
    }
}

void Editor::showImageMenu(const QString &path, const QPoint &globalPos)
{
    if (!m_vault)
        return;
    const QString abs = m_vault->absolutePath(path);
    QMenu menu(this);
    menu.addAction(tr("Copy image"), this, [abs] { QApplication::clipboard()->setImage(QImage(abs)); });
    menu.addAction(tr("Open in new tab"), this, [this, path] { Q_EMIT linkActivated(wikiHref(path), true); });
    if (m_fileActions) {
        menu.addSeparator();
        m_fileActions(&menu, path, true);
    }
    menu.exec(globalPos);
}

// First and last line of the callout around `block`, both invalid outside one.
QPair<QTextBlock, QTextBlock> Editor::calloutAt(const QTextBlock &block) const
{
    auto in = [](const QTextBlock &b) { return b.isValid() && b.userState() > 0 && (b.userState() & BlockState::Callout); };
    auto head = [](const QTextBlock &b) { return calloutHeadRe().match(b.text()).hasMatch(); };
    if (!in(block))
        return {};
    QTextBlock first = block;
    while (!head(first) && in(first.previous()))
        first = first.previous();
    QTextBlock last = block;
    while (in(last.next()) && !head(last.next()))
        last = last.next();
    return {first, last};
}

void Editor::paintCalloutHead(QPainter &p, const QTextBlock &block)
{
    const auto m = calloutHeadRe().match(block.text());
    QTextLayout *layout = block.layout();
    if (!m.hasMatch() || !layout || layout->lineCount() == 0)
        return;
    const QTextLine line = layout->lineAt(0);
    const QRectF r = blockBoundingGeometry(block).translated(contentOffset());
    const int at = int(block.text().indexOf(u"[!"_s));
    const double x = r.left() + line.cursorToX(at);
    const QRectF lineRect(r.left(), r.top() + line.y(), r.width(), line.height());
    const CalloutStyle style = calloutStyle(m.captured(1));
    const int side = qRound(qMin(lineRect.height() * 0.8, QFontMetricsF(font()).height() * 1.05));
    const qreal dpr = devicePixelRatioF();
    const QString key = style.icon + style.color + QString::number(side) + u'@' + QString::number(dpr);
    if (!m_icons.contains(key))
        m_icons.insert(key, Theme::image(style.icon, QColor(style.color), side, dpr));
    p.drawImage(QPointF(x - side - QFontMetricsF(font()).height() * 0.35, lineRect.center().y() - side / 2.0), m_icons[key]);
    if (!m.captured(2).trimmed().isEmpty())
        return;
    const QString type = m.captured(1);
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(QColor(style.color));
    p.drawText(QRectF(x, lineRect.top(), r.width() - x, lineRect.height()), Qt::AlignLeft | Qt::AlignVCenter,
               type.left(1).toUpper() + type.mid(1).toLower());
}
