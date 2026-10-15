#include "NoteView.h"

#include "Editor.h"
#include "Markdown.h"
#include "Reader.h"
#include "Theme.h"
#include "TextMerge.h"
#include "Vault.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBlock>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace {

double scrollRatio(const QScrollBar *bar)
{
    return bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.0;
}

void applyRatio(QScrollBar *bar, double ratio)
{
    bar->setValue(int(ratio * bar->maximum()));
}

} // namespace

NoteView::NoteView(Vault *vault, QWidget *parent)
    : QWidget(parent)
    , m_vault(vault)
{
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);

    auto toolButton = [this](const QString &icon, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setIcon(Theme::icon(icon));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_header = new QWidget(this);
    m_backButton = toolButton(u"chevron-left"_s, tr("Back (Alt+Left)"));
    m_forwardButton = toolButton(u"chevron-right"_s, tr("Forward (Alt+Right)"));
    m_crumb = new QLabel(m_header);
    m_crumb->setForegroundRole(QPalette::PlaceholderText);
    m_crumb->setAlignment(Qt::AlignCenter);
    m_crumb->setMinimumWidth(0);
    m_crumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_modeButton = toolButton(u"book"_s, QString());
    auto *more = toolButton(u"more"_s, tr("More options"));
    connect(more, &QToolButton::clicked, this, [this, more] { Q_EMIT moreRequested(more->mapToGlobal(QPoint(0, more->height()))); });
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(8, 6, 8, 0);
    headerLayout->setSpacing(2);
    headerLayout->addWidget(m_backButton);
    headerLayout->addWidget(m_forwardButton);
    headerLayout->addWidget(m_crumb, 1);
    headerLayout->addWidget(m_modeButton);
    headerLayout->addWidget(more);
    connect(m_backButton, &QToolButton::clicked, this, &NoteView::goBack);
    connect(m_forwardButton, &QToolButton::clicked, this, &NoteView::goForward);
    connect(m_modeButton, &QToolButton::clicked, this, [this] { setMode(m_mode == Edit ? Read : Edit); });

    m_titleBar = new QWidget(this);
    m_title = new QLineEdit(m_titleBar);
    m_title->setFrame(false);
    m_title->setStyleSheet(u"QLineEdit { background: transparent; border: none; padding: 0px; }"_s);
    auto *titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->addWidget(m_title);

    m_editor = new Editor(this);
    m_editor->setVault(vault);
    m_reader = new Reader(this);
    m_image = new QLabel;
    m_image->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_imageArea = new QScrollArea(this);
    m_imageArea->setFrameShape(QFrame::NoFrame);
    m_imageArea->setWidgetResizable(true);
    m_imageArea->setWidget(m_image);
    m_imageArea->setBackgroundRole(QPalette::Base);
    m_imageArea->viewport()->installEventFilter(this);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_editor);
    m_stack->addWidget(m_reader);
    m_stack->addWidget(m_imageArea);

    m_findBar = new QWidget(this);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Find in note"));
    m_findEdit->setClearButtonEnabled(true);
    auto *prev = toolButton(u"chevron-up"_s, tr("Previous match (Shift+Enter)"));
    auto *next = toolButton(u"chevron-down"_s, tr("Next match (Enter)"));
    auto *close = toolButton(u"close"_s, tr("Close (Esc)"));
    auto *findLayout = new QHBoxLayout;
    findLayout->setSpacing(2);
    findLayout->addWidget(m_findEdit, 1);
    findLayout->addWidget(prev);
    findLayout->addWidget(next);
    findLayout->addWidget(close);
    m_replaceRow = new QWidget(m_findBar);
    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setPlaceholderText(tr("Replace with"));
    auto *replaceOne = new QPushButton(tr("Replace"), m_replaceRow);
    auto *replaceAll = new QPushButton(tr("Replace all"), m_replaceRow);
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(4);
    replaceLayout->addWidget(m_replaceEdit, 1);
    replaceLayout->addWidget(replaceOne);
    replaceLayout->addWidget(replaceAll);
    auto *findColumn = new QVBoxLayout;
    findColumn->setSpacing(6);
    findColumn->addLayout(findLayout);
    findColumn->addWidget(m_replaceRow);
    auto *findOuter = new QVBoxLayout(m_findBar);
    findOuter->setContentsMargins(8, 6, 8, 6);
    findOuter->addLayout(findColumn);
    findLayout->setContentsMargins(0, 0, 0, 0);
    connect(replaceOne, &QPushButton::clicked, this, [this] { replace(false); });
    connect(replaceAll, &QPushButton::clicked, this, [this] { replace(true); });
    connect(m_replaceEdit, &QLineEdit::returnPressed, this, [this] { replace(false); });
    setTabOrder(m_findEdit, m_replaceEdit);
    setTabOrder(m_replaceEdit, replaceOne);
    setTabOrder(replaceOne, replaceAll);
    m_findBar->hide();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_header);
    layout->addWidget(m_titleBar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_findBar);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(700);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        save(false);
        Q_EMIT statsChanged();
    });
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(120);
    connect(&m_renderTimer, &QTimer::timeout, this, [this] {
        if (m_mode == Read && isNote())
            render();
    });

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading)
            m_saveTimer.start();
    });
    connect(m_editor, &Editor::linkActivated, this, &NoteView::handleHref);
    connect(m_editor, &Editor::hoverLink, this, &NoteView::hoverLink);
    connect(m_reader, &Reader::hoverLink, this, &NoteView::hoverLink);
    connect(m_editor, &Editor::foldsChanged, this, &NoteView::storeFolds);
    connect(m_editor, &Editor::hintChanged, this, &NoteView::hintChanged);
    connect(m_reader, &Reader::openRequested, this, &NoteView::handleHref);
    connect(m_reader, &Reader::imageRequested, this, [this](const QString &file, bool newTab) {
        Q_EMIT linkRequested(m_vault->relativePath(file), newTab);
    });
    connect(m_reader, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        handleHref(url.toString(QUrl::FullyEncoded), QApplication::keyboardModifiers() & Qt::ControlModifier);
    });
    connect(m_reader, &Reader::contentWidthChanged, this, [this] {
        if (m_reader->toHtml().contains(u"<img"_s))
            m_renderTimer.start();
    });

    connect(m_title, &QLineEdit::returnPressed, this, &NoteView::focusContent);
    connect(m_title, &QLineEdit::editingFinished, this, [this] {
        const QString name = m_title->text().trimmed();
        if (m_path.isEmpty() || name == Vault::baseName(m_path))
            return;
        if (name.isEmpty()) {
            resetTitle();
            return;
        }
        Q_EMIT renameRequested(m_path, name);
    });

    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] {
        find(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(prev, &QToolButton::clicked, this, [this] { find(true); });
    connect(next, &QToolButton::clicked, this, [this] { find(false); });
    connect(close, &QToolButton::clicked, this, [this] {
        m_findBar->hide();
        focusContent();
    });
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, close, &QToolButton::click);
}

bool NoteView::isNote() const
{
    return Vault::isNote(m_path);
}

QString NoteView::editorText() const
{
    // toPlainText() would turn non-breaking spaces into plain spaces.
    QString t = m_editor->document()->toRawText();
    t.replace(QChar::ParagraphSeparator, u'\n');
    return t;
}

void NoteView::open(const QString &path, bool pushHistory)
{
    if (path == m_path)
        return;
    const bool discarded = discardIfUntouched();
    if (!discarded)
        save();
    else if (!m_back.isEmpty() && m_back.last() == path)
        m_back.removeLast(); // Back would lead to `path` itself
    if (pushHistory && !m_path.isEmpty() && !discarded) {
        m_back << m_path;
        m_forward.clear();
    }
    load(path);
}

void NoteView::load(const QString &path)
{
    m_path = path;
    m_editor->setNotePath(path);
    resetTitle();
    m_findBar->hide();

    if (isNote()) {
        m_diskText = m_vault->readText(path);
        m_crlf = m_diskText.contains(u"\r\n"_s);
        m_loading = true;
        m_editor->setPlainText(m_crlf ? QString(m_diskText).replace(u"\r\n"_s, u"\n"_s) : m_diskText);
        m_editor->setFoldedLines(savedFolds());
        m_openCallouts.clear();
        m_loading = false;
        m_title->setReadOnly(false);
        if (m_mode == Read) {
            render();
            m_stack->setCurrentWidget(m_reader);
        } else {
            m_stack->setCurrentWidget(m_editor);
        }
    } else {
        m_diskText.clear();
        m_pixmap = QPixmap(m_vault->absolutePath(path));
        scaleImage();
        m_stack->setCurrentWidget(m_imageArea);
    }
    updateHeader();
    Q_EMIT pathChanged();
    Q_EMIT statsChanged();
}

// Folds are remembered per note, keyed by a hash because paths make poor settings keys.
QString NoteView::foldsKey() const
{
    return QString::fromLatin1(QCryptographicHash::hash((m_vault->root() + u'\n' + m_path).toUtf8(), QCryptographicHash::Sha1).toHex());
}

void NoteView::storeFolds()
{
    QSettings settings;
    settings.beginGroup(u"folds"_s);
    QVariantList lines;
    for (const int line : m_editor->foldedLines())
        lines << line;
    if (lines.isEmpty())
        settings.remove(foldsKey());
    else
        settings.setValue(foldsKey(), lines);
}

QList<int> NoteView::savedFolds() const
{
    QSettings settings;
    settings.beginGroup(u"folds"_s);
    QList<int> out;
    for (const QVariant &v : settings.value(foldsKey()).toList())
        out << v.toInt();
    return out;
}

void NoteView::updateHeader()
{
    m_backButton->setEnabled(!m_back.isEmpty());
    m_forwardButton->setEnabled(!m_forward.isEmpty());
    const QString dir = m_path.section(u'/', 0, -2);
    m_crumb->setText(dir.isEmpty() ? QString() : QString(dir).replace(u'/', u"  /  "_s));
    m_crumb->setToolTip(m_path);
    m_modeButton->setVisible(isNote());
    const bool editing = m_mode == Edit;
    m_modeButton->setIcon(Theme::icon(editing ? u"book"_s : u"pencil"_s));
    m_modeButton->setToolTip(editing ? tr("Switch to reading view (Ctrl+E)") : tr("Switch to editing (Ctrl+E)"));
}

void NoteView::resetTitle()
{
    const QSignalBlocker block(m_title);
    m_title->setText(m_path.isEmpty() ? QString() : (isNote() ? Vault::baseName(m_path) : m_path.section(u'/', -1)));
    m_title->setCursorPosition(0);
}

bool NoteView::save(bool commitTables)
{
    if (!isNote())
        return true;
    if (commitTables)
        m_editor->commitTables();
    m_saveTimer.stop();
    QString t = editorText();
    if (m_crlf)
        t.replace(u'\n', u"\r\n"_s);
    if (t == m_diskText)
        return true;
    if (!m_vault->writeNote(m_path, t))
        return false;
    m_diskText = t;
    storeFolds(); // line numbers as they are in the saved text
    Q_EMIT saved();
    return true;
}

bool NoteView::discardIfUntouched()
{
    if (m_newPath.isEmpty() || m_path != m_newPath || !editorText().trimmed().isEmpty())
        return false;
    m_saveTimer.stop();
    m_newPath.clear();
    QFile::remove(m_vault->absolutePath(m_path));
    m_vault->rescan();
    return true;
}

void NoteView::reloadFromDisk()
{
    if (!isNote()) {
        if (!m_path.isEmpty()) {
            m_pixmap = QPixmap(m_vault->absolutePath(m_path));
            scaleImage();
        }
        return;
    }
    const QString text = m_vault->readText(m_path);
    if (text == m_diskText)
        return;
    const QString base = QString(m_diskText).replace(u"\r\n"_s, u"\n"_s);
    const QString theirs = QString(text).replace(u"\r\n"_s, u"\n"_s);
    const QString ours = editorText();
    const bool edited = ours != base;
    QString merged;
    if (edited && !TextMerge::merge(base, ours, theirs, &merged)) {
        // Both sides changed the same text: ours stays in the note, theirs is kept as a copy.
        const QString dir = m_path.section(u'/', 0, -2);
        const QString name = u"%1 (conflict %2).md"_s.arg(Vault::baseName(m_path), QDateTime::currentDateTime().toString(u"yyyy-MM-dd HHmm"_s));
        const QString copy = m_vault->createNote(m_vault->uniquePath(dir.isEmpty() ? name : dir + u'/' + name), text);
        m_diskText = text;
        m_saveTimer.start();
        Q_EMIT notice(copy.isEmpty() ? tr("“%1” was changed outside Serat while you were editing it; your version is kept.").arg(Vault::baseName(m_path))
                                     : tr("“%1” was changed outside Serat while you were editing it. Your version is kept; the other one is in “%2”.")
                                           .arg(Vault::baseName(m_path), Vault::baseName(copy)));
        return;
    }
    // Only the stretch that changed is replaced, so the cursor, folds and undo history stay put.
    const TextMerge::Change change = TextMerge::between(ours, edited ? merged : theirs);
    m_diskText = text;
    m_crlf = text.contains(u"\r\n"_s);
    m_loading = !edited;
    QTextCursor c(m_editor->document());
    c.setPosition(change.from);
    c.setPosition(change.to, QTextCursor::KeepAnchor);
    c.insertText(change.text);
    m_loading = false;
    if (edited)
        m_saveTimer.start();
    if (m_mode == Read)
        render();
    Q_EMIT statsChanged();
}

bool NoteView::hasUnsavedChanges() const
{
    return isNote() && editorText() != QString(m_diskText).replace(u"\r\n"_s, u"\n"_s);
}

void NoteView::pathRenamed(const QString &from, const QString &to)
{
    for (QString &p : m_back)
        if (p == from)
            p = to;
    for (QString &p : m_forward)
        if (p == from)
            p = to;
    if (m_path != from)
        return;
    m_path = to;
    m_editor->setNotePath(to);
    resetTitle();
    updateHeader();
    Q_EMIT pathChanged();
}

void NoteView::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    if (isNote()) {
        if (mode == Read) {
            const double ratio = scrollRatio(m_editor->verticalScrollBar());
            save();
            render();
            m_stack->setCurrentWidget(m_reader);
            QTimer::singleShot(0, this, [this, ratio] { applyRatio(m_reader->verticalScrollBar(), ratio); });
        } else {
            const double ratio = scrollRatio(m_reader->verticalScrollBar());
            m_stack->setCurrentWidget(m_editor);
            QTimer::singleShot(0, this, [this, ratio] { applyRatio(m_editor->verticalScrollBar(), ratio); });
        }
        focusContent();
    }
    updateHeader();
    Q_EMIT modeChanged();
}

void NoteView::goBack()
{
    if (m_back.isEmpty())
        return;
    if (!discardIfUntouched()) {
        save();
        m_forward.prepend(m_path);
    }
    load(m_back.takeLast());
}

void NoteView::goForward()
{
    if (m_forward.isEmpty())
        return;
    if (!discardIfUntouched()) {
        save();
        m_back << m_path;
    }
    load(m_forward.takeFirst());
}

void NoteView::render()
{
    RenderOptions opt;
    opt.vault = m_vault;
    opt.notePath = m_path;
    opt.contentWidth = qMax(200, m_reader->contentWidth());
    opt.palette = palette();
    opt.dark = palette().color(QPalette::Base).lightness() < 128;
    opt.toggledCallouts = m_openCallouts;
    const RenderResult result = renderMarkdown(editorText(), opt);
    m_codeBlocks = result.codeBlocks;

    const int scroll = m_reader->verticalScrollBar()->value();
    m_reader->document()->setDefaultFont(m_font);
    m_reader->document()->setDefaultStyleSheet(markdownStyleSheet(palette(), m_font.pointSizeF()));
    m_reader->setHtml(result.html);
    m_reader->verticalScrollBar()->setValue(scroll);
}

void NoteView::relayout()
{
    for (QWidget *w : {static_cast<QWidget *>(m_editor), static_cast<QWidget *>(m_reader)}) {
        QResizeEvent e(w->size(), w->size());
        QApplication::sendEvent(w, &e);
    }
}

void NoteView::refreshRender()
{
    if (m_mode == Read && isNote())
        m_renderTimer.start();
}

void NoteView::handleHref(const QString &href, bool newTab)
{
    const QString scheme = href.section(u':', 0, 0);
    if (scheme == u"wiki") {
        const QString target = QUrl::fromPercentEncoding(href.mid(5).toUtf8());
        if (target.startsWith(u'#'))
            jumpToHeading(target.mid(1));
        else
            Q_EMIT linkRequested(target, newTab);
        return;
    }
    if (scheme == u"task") {
        toggleTask(href.mid(5).toInt());
        return;
    }
    if (scheme == u"callout") {
        const int index = href.mid(8).toInt();
        if (!m_openCallouts.remove(index))
            m_openCallouts.insert(index);
        render();
        return;
    }
    if (scheme == u"copy") {
        const int i = href.mid(5).toInt();
        if (i >= 0 && i < m_codeBlocks.size()) {
            QApplication::clipboard()->setText(m_codeBlocks.at(i));
            QToolTip::showText(QCursor::pos(), tr("Copied"), this);
        }
        return;
    }
    if (href.startsWith(u'#')) {
        m_reader->scrollToAnchor(href.mid(1));
        return;
    }
    const QUrl url(href);
    if (url.scheme().isEmpty()) {
        if (href.startsWith(u"www."))
            QDesktopServices::openUrl(QUrl(u"https://"_s + href));
        else
            Q_EMIT linkRequested(QUrl::fromPercentEncoding(href.toUtf8()), newTab);
        return;
    }
    QDesktopServices::openUrl(url);
}

void NoteView::toggleTask(int offset)
{
    const QByteArray utf8 = editorText().toUtf8();
    if (offset < 0 || offset >= utf8.size())
        return;
    const QChar mark = utf8.at(offset) == ' ' ? u'x' : u' ';
    const int pos = int(QString::fromUtf8(utf8.left(offset)).size());
    QTextCursor c(m_editor->document());
    c.setPosition(pos);
    c.setPosition(pos + 1, QTextCursor::KeepAnchor);
    c.insertText(QString(mark));
    render();
}

void NoteView::jumpToHeading(const QString &heading)
{
    if (heading.startsWith(u'^')) {
        const QRegularExpression id(u"\\s\\^"_s + QRegularExpression::escape(heading.mid(1)) + u"\\s*$"_s);
        for (QTextBlock b = m_editor->document()->begin(); b.isValid(); b = b.next())
            if (id.match(b.text()).hasMatch())
                return jumpToLine(b.blockNumber());
        return;
    }
    const QString want = headingAnchor(heading);
    if (m_mode == Read && isNote()) {
        m_reader->scrollToAnchor(want);
        return;
    }
    static const QRegularExpression h(u"^#{1,6}\\s+(.*?)\\s*#*\\s*$"_s);
    for (QTextBlock b = m_editor->document()->begin(); b.isValid(); b = b.next()) {
        const auto m = h.match(b.text());
        if (m.hasMatch() && headingAnchor(m.captured(1)) == want) {
            m_editor->goToLine(b.blockNumber());
            return;
        }
    }
}

void NoteView::jumpToLine(int line)
{
    if (!isNote() || line < 0)
        return;
    if (m_mode == Edit) {
        m_editor->goToLine(line);
        return;
    }
    // Reading view: find the rendered text of that line.
    static const QRegularExpression prefix(u"^\\s*(#{1,6}\\s+|>\\s*|[-*+]\\s+(\\[[ xX]\\]\\s+)?|\\d+[.)]\\s+)*"_s);
    static const QRegularExpression markup(u"[*_`~=\\[\\]]"_s);
    static const QRegularExpression blockId(u"\\s\\^[A-Za-z0-9-]+\\s*$"_s);
    QString snippet = m_editor->document()->findBlockByNumber(line).text();
    snippet.remove(blockId);
    snippet.remove(prefix);
    snippet.remove(markup);
    snippet = snippet.trimmed().left(40);
    QTextCursor start(m_reader->document());
    m_reader->setTextCursor(start);
    if (snippet.isEmpty() || !m_reader->find(snippet))
        applyRatio(m_reader->verticalScrollBar(), double(line) / qMax(1, m_editor->blockCount()));
}

void NoteView::showFind(bool replace)
{
    if (!isNote())
        return;
    if (replace && m_mode == Read)
        setMode(Edit);
    m_replaceRow->setVisible(replace);
    const QString selected = m_mode == Edit ? m_editor->textCursor().selectedText() : m_reader->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator))
        m_findEdit->setText(selected);
    m_findBar->show();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void NoteView::find(bool backward)
{
    const QString q = m_findEdit->text();
    if (q.isEmpty())
        return;
    const QTextDocument::FindFlags flags = backward ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    auto run = [&](auto *view) {
        if (view->find(q, flags))
            return true;
        QTextCursor c = view->textCursor();
        c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        view->setTextCursor(c);
        return view->find(q, flags);
    };
    const bool found = m_mode == Edit ? run(m_editor) : run(static_cast<QTextBrowser *>(m_reader));
    QPalette pal = m_findEdit->palette();
    pal.setColor(QPalette::Base, found ? palette().color(QPalette::Base) : mix(palette().color(QPalette::Base), Qt::red, 0.25));
    m_findEdit->setPalette(pal);
}

void NoteView::replace(bool all)
{
    const QString q = m_findEdit->text();
    if (q.isEmpty() || m_mode != Edit)
        return;
    const QString with = m_replaceEdit->text();
    if (!all) {
        QTextCursor c = m_editor->textCursor();
        if (c.selectedText().compare(q, Qt::CaseInsensitive) == 0)
            c.insertText(with);
        find(false);
        return;
    }
    QTextCursor edit(m_editor->document());
    edit.beginEditBlock();
    int count = 0;
    for (QTextCursor hit = m_editor->document()->find(q); !hit.isNull(); hit = m_editor->document()->find(q, hit)) {
        hit.insertText(with);
        ++count;
    }
    edit.endEditBlock();
    QToolTip::showText(m_replaceEdit->mapToGlobal(QPoint(0, -m_replaceEdit->height())), tr("%n replaced", nullptr, count), m_replaceEdit);
}

void NoteView::focusTitle()
{
    m_title->setFocus();
    m_title->selectAll();
}

void NoteView::focusContent()
{
    if (!isNote())
        return;
    if (m_mode == Edit)
        m_editor->setFocus();
    else
        m_reader->setFocus();
}

void NoteView::applyFont(const QFont &font)
{
    m_font = font;
    m_editor->applyFont(font);
    QFont title = font;
    title.setPointSizeF(font.pointSizeF() * 1.8);
    title.setBold(true);
    m_title->setFont(title);
    if (m_mode == Read && isNote())
        render();
}

void NoteView::setLivePreview(bool on)
{
    m_editor->setLivePreview(on);
}

void NoteView::setLinkCandidates(const QStringList &names)
{
    m_editor->setLinkCandidates(names);
}

QString NoteView::statsText() const
{
    if (!isNote())
        return {};
    const QString text = editorText();
    static const QRegularExpression word(u"\\S+"_s);
    qsizetype words = 0;
    for (auto it = word.globalMatch(text); it.hasNext(); it.next())
        ++words;
    const QLocale locale;
    const QString w = words == 1 ? tr("1 word") : tr("%1 words").arg(locale.toString(words));
    const QString c = text.size() == 1 ? tr("1 character") : tr("%1 characters").arg(locale.toString(text.size()));
    return w + u"  ·  "_s + c;
}

void NoteView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateMargins();
}

bool NoteView::eventFilter(QObject *obj, QEvent *e)
{
    // A tab opened in the background lays out its image area later than the tab itself.
    if (obj == m_imageArea->viewport() && e->type() == QEvent::Resize)
        scaleImage();
    return QWidget::eventFilter(obj, e);
}

void NoteView::updateMargins()
{
    const int scroll = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int margin = readableMargin(width() - scroll);
    // +2 aligns the title with the editor text.
    m_titleBar->layout()->setContentsMargins(margin + 2, 12, margin + scroll, 4);
}

void NoteView::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        refreshRender();
}

void NoteView::scaleImage()
{
    if (m_pixmap.isNull() || isNote()) {
        m_image->clear();
        return;
    }
    const int w = m_imageArea->viewport()->width() - 48;
    if (w <= 0 || m_pixmap.width() <= w) {
        m_image->setPixmap(m_pixmap);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap fitted = m_pixmap.scaledToWidth(qRound(w * dpr), Qt::SmoothTransformation);
    fitted.setDevicePixelRatio(dpr);
    m_image->setPixmap(fitted);
}
