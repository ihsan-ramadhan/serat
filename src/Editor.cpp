#include "Editor.h"

#include "Markdown.h"
#include "MarkdownHighlighter.h"
#include "Vault.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
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

} // namespace

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
    retheme();
}

void Editor::setLinkCandidates(const QStringList &names)
{
    // Every autosave refreshes the vault; resetting the model would drop the
    // selected row of an open completion popup.
    if (names == m_candidates->stringList())
        return;
    m_candidates->setStringList(names);
    if (m_completer->popup()->isVisible())
        updateCompletion();
}

void Editor::applyFont(const QFont &font)
{
    setFont(font);
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(u' ') * 4);
    retheme();
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
    }
    QPlainTextEdit::paintEvent(e);
}

void Editor::keyPressEvent(QKeyEvent *e)
{
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
    // Follow the note's own indentation: tabs by default (Obsidian), or the
    // width of the first space-indented list item.
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
    const QString prefix = before.mid(open + 2);
    if (prefix.contains(u'|') || prefix.contains(u'#') || prefix.size() > 100) {
        popup->hide();
        return;
    }
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
    c.beginEditBlock();
    c.setPosition(c.block().position() + int(open) + 2, QTextCursor::KeepAnchor);
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
    const bool onLink = (e->modifiers() & Qt::ControlModifier) && !linkAt(cursorForPosition(e->position().toPoint())).isEmpty();
    viewport()->setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
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
