#include "MainWindow.h"

#include "CommandPalette.h"
#include "Editor.h"
#include "NoteView.h"
#include "Panels.h"
#include "PathActions.h"
#include "QuickSwitcher.h"
#include "Theme.h"
#include "Vault.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStylePainter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

QString dirOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

QString join(const QString &dir, const QString &name)
{
    return dir.isEmpty() ? name : dir + u'/' + name;
}

QStringList obsidianVaults()
{
    QStringList configs;
#if defined(Q_OS_WIN)
    configs << qEnvironmentVariable("APPDATA") + u"/obsidian/obsidian.json"_s;
#elif defined(Q_OS_MACOS)
    configs << QDir::homePath() + u"/Library/Application Support/obsidian/obsidian.json"_s;
#else
    configs << QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/obsidian/obsidian.json"_s
            << QDir::homePath() + u"/.var/app/md.obsidian.Obsidian/config/obsidian/obsidian.json"_s;
#endif
    QStringList vaults;
    for (const QString &config : std::as_const(configs)) {
        QFile f(config);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object().value(u"vaults").toObject();
        for (const QJsonValue &v : all) {
            const QString path = v.toObject().value(u"path").toString();
            if (QFileInfo(path).isDir() && !vaults.contains(path))
                vaults << path;
        }
    }
    return vaults;
}

// QTabBar left-aligns icons even when a tab has no text; draw them centred.
class IconTabBar : public QTabBar
{
public:
    using QTabBar::QTabBar;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        for (int i = 0; i < count(); ++i) {
            QStyleOptionTab opt;
            initStyleOption(&opt, i);
            p.drawControl(QStyle::CE_TabBarTabShape, opt);
            QRect r(QPoint(), iconSize());
            r.moveCenter(opt.rect.center());
            const QIcon::Mode mode = (opt.state & QStyle::State_MouseOver) ? QIcon::Active : QIcon::Normal;
            opt.icon.paint(&p, r, Qt::AlignCenter, mode, (opt.state & QStyle::State_Selected) ? QIcon::On : QIcon::Off);
            if (m_keyboardFocus && hasFocus() && i == currentIndex()) {
                p.setPen(QPen(palette().color(QPalette::Accent), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-5, -4, 4, 3), 4, 4);
            }
        }
    }

    void focusInEvent(QFocusEvent *e) override
    {
        m_keyboardFocus = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;
        QTabBar::focusInEvent(e);
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        m_keyboardFocus = false;
        QTabBar::focusOutEvent(e);
    }

private:
    bool m_keyboardFocus = false;
};

class PanelTabs : public QTabWidget
{
public:
    PanelTabs() { setTabBar(new IconTabBar); }
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    buildActions();

    QSettings s;
    m_fontDelta = s.value(u"fontDelta"_s, 0.0).toDouble();
    m_recentCommands = s.value(u"recentCommands"_s).toStringList();
    if (!restoreGeometry(s.value(u"geometry"_s).toByteArray()))
        resize(1280, 820);
    m_split->restoreState(s.value(u"splitter"_s).toByteArray());

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MainWindow::onVaultChanged);
}

void MainWindow::buildUi()
{
    m_tree = new FileTree;
    m_search = new SearchPanel;
    auto panelTabs = [](std::initializer_list<std::tuple<QWidget *, const char16_t *, QString>> pages) {
        auto *w = new PanelTabs;
        w->setDocumentMode(true);
        w->tabBar()->setObjectName(u"panels"_s);
        // Document mode turns expanding off.
        w->tabBar()->setExpanding(true);
        w->tabBar()->setDrawBase(false);
        w->setIconSize(QSize(18, 18));
        for (const auto &[page, icon, tip] : pages) {
            const int i = w->addTab(page, Theme::icon(QString::fromUtf16(icon)), QString());
            w->setTabToolTip(i, tip);
        }
        return w;
    };
    m_left = panelTabs({{buildFilesPage(), u"folder", tr("Files")}, {m_search, u"search", tr("Search (Ctrl+Shift+F)")}});

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setObjectName(u"notes"_s);
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->setMovable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->installEventFilter(this);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested, this, &MainWindow::showTabMenu);
    auto *plus = new QToolButton;
    plus->setIcon(Theme::icon(u"file-plus"_s));
    plus->setIconSize(QSize(18, 18));
    plus->setAutoRaise(true);
    plus->setToolTip(tr("New note (Ctrl+N)"));
    auto *corner = new QWidget;
    auto *cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 6, 4); // aligns with the tab titles
    cornerLayout->addWidget(plus, 0, Qt::AlignVCenter);
    m_tabs->setCornerWidget(corner, Qt::TopRightCorner);
    connect(plus, &QToolButton::clicked, this, [this] { newNote({}); });

    m_backlinks = new BacklinksPanel;
    m_outgoing = new OutgoingPanel;
    m_outline = new OutlinePanel;
    m_right = panelTabs({{m_backlinks, u"backlinks", tr("Backlinks")},
                         {m_outgoing, u"outgoing", tr("Outgoing links")},
                         {m_outline, u"outline", tr("Outline")}});

    m_split = new QSplitter;
    m_split->addWidget(m_left);
    m_split->addWidget(m_tabs);
    m_split->addWidget(m_right);
    m_split->setStretchFactor(1, 1);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(1);
    m_split->setSizes({240, 900, 260});

    m_welcome = new QWidget;
    auto *welcomeOuter = new QVBoxLayout(m_welcome);
    auto *card = new QWidget;
    card->setFixedWidth(440);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(6);
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(u":/serat.svg"_s).pixmap(56, 56));
    auto *title = new QLabel(u"Serat"_s);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 2.2);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    auto *subtitle = new QLabel(tr("Plain Markdown notes in a folder. Opens Obsidian vaults as they are."));
    subtitle->setWordWrap(true);
    subtitle->setForegroundRole(QPalette::PlaceholderText);
    auto *openButton = new QPushButton(tr("Open folder as vault…"));
    openButton->setObjectName(u"primary"_s);
    openButton->setDefault(true);
    m_welcomeList = new QVBoxLayout;
    m_welcomeList->setSpacing(2);
    cardLayout->addWidget(logo);
    cardLayout->addSpacing(4);
    cardLayout->addWidget(title);
    cardLayout->addWidget(subtitle);
    cardLayout->addSpacing(18);
    cardLayout->addLayout(m_welcomeList);
    cardLayout->addSpacing(14);
    cardLayout->addWidget(openButton, 0, Qt::AlignLeft);
    welcomeOuter->addStretch();
    welcomeOuter->addWidget(card, 0, Qt::AlignHCenter);
    welcomeOuter->addStretch(2);
    connect(openButton, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Open folder as vault"), QDir::homePath());
        if (!dir.isEmpty())
            openVault(dir);
    });

    m_pages = new QStackedWidget;
    m_pages->addWidget(m_welcome);
    m_pages->addWidget(m_split);
    setCentralWidget(m_pages);

    m_vaultLabel = new QLabel;
    m_statsLabel = new QLabel;
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(m_vaultLabel);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentChanged);

    connect(m_tree, &FileTree::openRequested, this, [this](const QString &p, bool newTab) { openPath(p, newTab); });
    connect(m_tree, &FileTree::newNoteRequested, this, &MainWindow::newNote);
    connect(m_tree, &FileTree::newFolderRequested, this, &MainWindow::newFolder);
    connect(m_tree, &FileTree::renameRequested, this, &MainWindow::promptRename);
    connect(m_tree, &FileTree::deleteRequested, this, &MainWindow::deletePath);
    connect(m_tree, &FileTree::moveRequested, this, &MainWindow::movePath);
    connect(m_tree, &FileTree::moveToRequested, this, &MainWindow::chooseFolderAndMove);
    connect(m_tree, &FileTree::duplicateRequested, this, &MainWindow::duplicatePath);
    connect(m_tree, &FileTree::searchInFolderRequested, this, [this](const QString &dir) { searchVault(u"path:\""_s + dir + u"/\" "_s); });
    connect(m_search, &SearchPanel::openRequested, this,
            [this](const QString &p, int line, bool newTab) { openPath(p, newTab, {}, line); });
    connect(m_backlinks, &BacklinksPanel::openRequested, this,
            [this](const QString &p, int line, bool newTab) { openPath(p, newTab, {}, line); });
    connect(m_outgoing, &OutgoingPanel::linkRequested, this, [this](const QString &target, bool newTab) {
        openLink(target, currentView() ? currentView()->path() : QString(), newTab);
    });
    connect(m_outline, &OutlinePanel::headingActivated, this, [this](const QString &heading) {
        if (NoteView *v = currentView())
            v->jumpToHeading(heading);
    });
}

void MainWindow::buildActions()
{
    auto add = [this](QMenu *menu, const QString &id, const QString &text, const QKeySequence &key, auto fn,
                      bool needsVault = true) {
        QAction *a = menu->addAction(text);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        if (needsVault)
            m_vaultActions << a;
        m_commands.insert(id, a);
        return a;
    };
    auto withView = [this](auto fn) {
        return [this, fn] {
            if (NoteView *v = currentView())
                fn(v);
        };
    };
    // Editing commands act on the note's text, not on a table cell, the title or the find bar.
    auto withEditor = [this](auto fn) {
        return [this, fn] {
            NoteView *v = currentView();
            if (!v || !v->isNote() || v->mode() == NoteView::Read)
                return;
            QWidget *focus = QApplication::focusWidget();
            if (focus && focus != v->editor() && v->isAncestorOf(focus))
                return;
            v->editor()->setFocus();
            fn(v->editor());
        };
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    add(file, u"file.new"_s, tr("New note"), QKeySequence::New, [this] { newNote({}); });
    add(file, u"file.newFolder"_s, tr("New folder…"), {}, [this] { newFolder({}); });
    file->addSeparator();
    add(file, u"vault.open"_s, tr("Open vault…"), QKeySequence(u"Ctrl+Shift+O"_s), [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Open folder as vault"),
                                                              m_vault ? m_vault->root() : QDir::homePath());
        if (!dir.isEmpty())
            openVault(dir);
    }, false);
    add(file, u"vault.close"_s, tr("Close vault"), {}, [this] { showWelcome(); });
    file->addSeparator();
    add(file, u"file.copy"_s, tr("Make a copy of the current file"), {}, withView([this](NoteView *v) { duplicatePath(v->path()); }));
    add(file, u"file.move"_s, tr("Move current file to another folder…"), {},
        withView([this](NoteView *v) { chooseFolderAndMove(v->path()); }));
    QMenu *copyPath = file->addMenu(tr("Copy current file path"));
    auto copy = [this, withView](auto text) {
        return withView([this, text](NoteView *v) { QApplication::clipboard()->setText(text(*m_vault, v->path())); });
    };
    add(copyPath, u"copy.obsidian"_s, tr("as Obsidian URL"), {},
        copy([](const Vault &vault, const QString &p) { return PathActions::obsidianUrl(vault, p).toString(QUrl::FullyEncoded); }));
    add(copyPath, u"copy.serat"_s, tr("as Serat link"), {},
        copy([](const Vault &vault, const QString &p) { return PathActions::seratUrl(vault, p).toString(QUrl::FullyEncoded); }));
    add(copyPath, u"copy.vault"_s, tr("from vault folder"), {}, copy([](const Vault &, const QString &p) { return p; }));
    add(copyPath, u"copy.root"_s, tr("from system root"), {},
        copy([](const Vault &vault, const QString &p) { return QDir::toNativeSeparators(vault.absolutePath(p)); }));
    add(file, u"file.defaultApp"_s, tr("Open in default app"), {},
        withView([this](NoteView *v) { QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(v->path()))); }));
    add(file, u"file.explorer"_s, tr("Show in system explorer"), {}, withView([this](NoteView *v) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_vault->absolutePath(v->path())).absolutePath()));
    }));
    add(file, u"file.reveal"_s, tr("Reveal current file in navigation"), {}, withView([this](NoteView *v) { revealInTree(v->path()); }));
    add(file, u"file.delete"_s, tr("Delete current file"), {}, withView([this](NoteView *v) { deletePath(v->path()); }));
    file->addSeparator();
    add(file, u"file.save"_s, tr("Save"), QKeySequence::Save, [this] { saveAll(); });
    add(file, u"tab.close"_s, tr("Close tab"), QKeySequence(u"Ctrl+W"_s), [this] {
        if (m_tabs->count())
            closeTab(m_tabs->currentIndex());
    });
    file->addSeparator();
    add(file, u"app.quit"_s, tr("Quit"), QKeySequence::Quit, [this] { close(); }, false);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, u"edit.find"_s, tr("Find…"), QKeySequence::Find, withView([](NoteView *v) { v->showFind(false); }));
    add(edit, u"edit.replace"_s, tr("Replace…"), QKeySequence(u"Ctrl+H"_s), withView([](NoteView *v) { v->showFind(true); }));
    edit->addSeparator();
    add(edit, u"edit.checkbox"_s, tr("Toggle checkbox status"), QKeySequence(u"Ctrl+L"_s), withEditor([](Editor *e) { e->toggleCheckbox(); }));
    add(edit, u"edit.deleteParagraph"_s, tr("Delete paragraph"), QKeySequence(u"Ctrl+D"_s), withEditor([](Editor *e) { e->deleteParagraph(); }));
    add(edit, u"edit.pastePlain"_s, tr("Paste as plain text"), QKeySequence(u"Ctrl+Shift+V"_s),
        withEditor([](Editor *e) { e->pasteAsPlainText(); }));
    edit->addSeparator();
    add(edit, u"link.follow"_s, tr("Follow link under cursor"), QKeySequence(u"Alt+Return"_s),
        withEditor([](Editor *e) { e->followLinkAtCursor(false); }));
    add(edit, u"link.followTab"_s, tr("Open link under cursor in new tab"), QKeySequence(u"Ctrl+Return"_s),
        withEditor([](Editor *e) { e->followLinkAtCursor(true); }));
    edit->addSeparator();
    add(edit, u"file.rename"_s, tr("Rename file"), QKeySequence(u"F2"_s), withView([](NoteView *v) { v->focusTitle(); }));

    QMenu *format = menuBar()->addMenu(tr("F&ormat"));
    add(format, u"format.bold"_s, tr("Bold"), QKeySequence::Bold, withEditor([](Editor *e) { e->toggleWrap(u"**"_s); }));
    add(format, u"format.italic"_s, tr("Italic"), QKeySequence::Italic, withEditor([](Editor *e) { e->toggleWrap(u"*"_s); }));
    add(format, u"format.strike"_s, tr("Strikethrough"), {}, withEditor([](Editor *e) { e->toggleWrap(u"~~"_s); }));
    add(format, u"format.highlight"_s, tr("Highlight"), {}, withEditor([](Editor *e) { e->toggleWrap(u"=="_s); }));
    format->addSeparator();
    add(format, u"format.code"_s, tr("Code"), {}, withEditor([](Editor *e) { e->toggleWrap(u"`"_s); }));
    add(format, u"format.comment"_s, tr("Comment"), QKeySequence(u"Ctrl+/"_s), withEditor([](Editor *e) { e->toggleWrap(u"%%"_s); }));
    format->addSeparator();
    add(format, u"format.clear"_s, tr("Clear formatting"), {}, withEditor([](Editor *e) { e->clearFormatting(); }));
    format->addSeparator();
    QMenu *paragraph = format->addMenu(tr("Paragraph"));
    using P = Editor::Paragraph;
    add(paragraph, u"para.bullet"_s, tr("Bullet list"), {}, withEditor([](Editor *e) { e->setParagraph(P::Bullet); }));
    add(paragraph, u"para.numbered"_s, tr("Numbered list"), {}, withEditor([](Editor *e) { e->setParagraph(P::Numbered); }));
    add(paragraph, u"para.task"_s, tr("Task list"), {}, withEditor([](Editor *e) { e->setParagraph(P::Task); }));
    paragraph->addSeparator();
    for (int level = 1; level <= 6; ++level)
        add(paragraph, u"para.h%1"_s.arg(level), tr("Heading %1").arg(level), {},
            withEditor([level](Editor *e) { e->setParagraph(P::Heading, level); }));
    add(paragraph, u"para.body"_s, tr("Body"), {}, withEditor([](Editor *e) { e->setParagraph(P::Body); }));
    paragraph->addSeparator();
    add(paragraph, u"para.quote"_s, tr("Quote"), {}, withEditor([](Editor *e) { e->setParagraph(P::Quote); }));

    QMenu *insert = menuBar()->addMenu(tr("&Insert"));
    add(insert, u"link.internal"_s, tr("Add link"), {}, withEditor([](Editor *e) { e->addInternalLink(); }));
    add(insert, u"link.external"_s, tr("Add external link"), QKeySequence(u"Ctrl+K"_s), withEditor([](Editor *e) { e->insertMarkdownLink(); }));
    insert->addSeparator();
    add(insert, u"insert.table"_s, tr("Table"), {}, withEditor([](Editor *e) { e->insertTable(); }));
    add(insert, u"insert.callout"_s, tr("Callout"), {}, withEditor([](Editor *e) { e->insertCallout(); }));
    add(insert, u"insert.rule"_s, tr("Horizontal rule"), {}, withEditor([](Editor *e) { e->insertRule(); }));
    add(insert, u"insert.codeblock"_s, tr("Code block"), {}, withEditor([](Editor *e) { e->insertCodeBlock(); }));
    insert->addSeparator();
    add(insert, u"insert.property"_s, tr("Add file property"), QKeySequence(u"Ctrl+;"_s), [this] {
        // Unlike the other editor commands this one also runs from inside the properties panel.
        if (NoteView *v = currentView(); v && v->isNote() && v->mode() == NoteView::Edit)
            v->editor()->addProperty();
    });

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    add(go, u"app.palette"_s, tr("Command palette"), QKeySequence(u"Ctrl+P"_s), [this] { commandPalette(); });
    add(go, u"app.switcher"_s, tr("Quick switcher"), QKeySequence(u"Ctrl+O"_s), [this] { quickSwitcher(); });
    add(go, u"app.search"_s, tr("Search vault"), QKeySequence(u"Ctrl+Shift+F"_s), [this] {
        QString selected;
        if (NoteView *v = currentView(); v && v->isNote() && v->mode() == NoteView::Edit)
            selected = v->editor()->textCursor().selectedText();
        searchVault(selected.contains(QChar::ParagraphSeparator) ? QString() : selected);
    });
    go->addSeparator();
    add(go, u"tab.new"_s, tr("New tab"), QKeySequence(u"Ctrl+T"_s), [this] { quickSwitcher(true); });
    add(go, u"tab.reopen"_s, tr("Reopen closed tab"), QKeySequence(u"Ctrl+Shift+T"_s), [this] {
        while (!m_closedTabs.isEmpty()) {
            const QString path = m_closedTabs.takeLast();
            if (m_vault->exists(path))
                return openPath(path, true);
        }
    });
    go->addSeparator();
    QAction *back = add(go, u"nav.back"_s, tr("Back"), QKeySequence(u"Alt+Left"_s), withView([](NoteView *v) { v->goBack(); }));
    // A key listed twice (QKeySequence::Back is Alt+Left) makes the shortcut ambiguous.
    auto unique = [](QList<QKeySequence> keys) {
        QList<QKeySequence> out;
        for (const QKeySequence &k : keys)
            if (!k.isEmpty() && !out.contains(k))
                out << k;
        return out;
    };
    back->setShortcuts(unique(QList<QKeySequence>{QKeySequence(u"Alt+Left"_s), QKeySequence(u"Ctrl+Alt+Left"_s)}
                              + QKeySequence::keyBindings(QKeySequence::Back)));
    QAction *fwd = add(go, u"nav.forward"_s, tr("Forward"), QKeySequence(u"Alt+Right"_s), withView([](NoteView *v) { v->goForward(); }));
    fwd->setShortcuts(unique(QList<QKeySequence>{QKeySequence(u"Alt+Right"_s), QKeySequence(u"Ctrl+Alt+Right"_s)}
                             + QKeySequence::keyBindings(QKeySequence::Forward)));
    go->addSeparator();
    add(go, u"tab.next"_s, tr("Next tab"), QKeySequence(u"Ctrl+Tab"_s), [this] {
        if (m_tabs->count())
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    })->setShortcuts({QKeySequence(u"Ctrl+Tab"_s), QKeySequence(u"Ctrl+PgDown"_s)});
    add(go, u"tab.previous"_s, tr("Previous tab"), QKeySequence(u"Ctrl+Shift+Tab"_s), [this] {
        if (m_tabs->count())
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    })->setShortcuts({QKeySequence(u"Ctrl+Shift+Tab"_s), QKeySequence(u"Ctrl+PgUp"_s)});
    QMenu *goTab = go->addMenu(tr("Go to tab"));
    for (int n = 1; n <= 9; ++n)
        add(goTab, u"tab.%1"_s.arg(n), n == 9 ? tr("Last tab") : tr("Tab %1").arg(n), QKeySequence(u"Ctrl+%1"_s.arg(n)), [this, n] {
            if (m_tabs->count())
                m_tabs->setCurrentIndex(n == 9 ? m_tabs->count() - 1 : qMin(n - 1, m_tabs->count() - 1));
        });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    add(view, u"view.reading"_s, tr("Toggle reading view"), QKeySequence(u"Ctrl+E"_s), withView([](NoteView *v) {
        v->setMode(v->mode() == NoteView::Edit ? NoteView::Read : NoteView::Edit);
    }));
    QAction *live = add(view, u"view.live"_s, tr("Live preview"), QKeySequence(u"Ctrl+Shift+E"_s), [this](bool on) {
        m_livePreview = on;
        QSettings().setValue(u"livePreview"_s, on);
        for (NoteView *v : views())
            v->setLivePreview(on);
    });
    live->setCheckable(true);
    m_livePreview = QSettings().value(u"livePreview"_s, true).toBool();
    live->setChecked(m_livePreview);
    live->setToolTip(tr("Hide Markdown markup except where the cursor is"));
    view->addSeparator();
    add(view, u"view.left"_s, tr("Toggle left sidebar"), QKeySequence(u"Ctrl+\\"_s), [this] { m_left->setVisible(!m_left->isVisible()); });
    add(view, u"view.right"_s, tr("Toggle right sidebar"), QKeySequence(u"Ctrl+Shift+\\"_s), [this] { m_right->setVisible(!m_right->isVisible()); });
    view->addSeparator();
    add(view, u"view.zoomIn"_s, tr("Zoom in"), QKeySequence::ZoomIn, [this] { zoom(1); })->setShortcuts({QKeySequence::ZoomIn, QKeySequence(u"Ctrl+="_s)});
    add(view, u"view.zoomOut"_s, tr("Zoom out"), QKeySequence::ZoomOut, [this] { zoom(-1); });
    add(view, u"view.zoomReset"_s, tr("Reset zoom"), QKeySequence(u"Ctrl+0"_s), [this] { zoom(-m_fontDelta); });
    view->addSeparator();
    QMenu *theme = view->addMenu(tr("Theme"));
    auto *themes = new QActionGroup(theme);
    const std::pair<QString, QString> modes[] = {{u"system"_s, tr("Follow system")}, {u"light"_s, tr("Light")}, {u"dark"_s, tr("Dark")}};
    for (const auto &[mode, label] : modes) {
        QAction *a = theme->addAction(label, this, [mode] { Theme::setPreference(mode); });
        a->setCheckable(true);
        a->setChecked(Theme::preference() == mode);
        themes->addAction(a);
    }

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    add(help, u"app.about"_s, tr("About Serat"), {}, [this] {
        QMessageBox::about(this, tr("About Serat"),
                           tr("<h3>Serat %1</h3><p>Native Markdown notes, compatible with Obsidian vaults.</p>"
                              "<p>Ctrl+click a link in the editor to follow it, Ctrl+E switches to the reading view.</p>"
                              "<p>Inspired by and compatible with Obsidian vaults; not affiliated with Obsidian.<br>"
                              "Built with Qt, md4c and KSyntaxHighlighting. Released under the MIT license.</p>")
                               .arg(QStringLiteral(SERAT_VERSION)));
    }, false);
}

void MainWindow::showWelcome()
{
    if (m_vault) {
        for (NoteView *v : views())
            v->discardIfUntouched();
        saveAll();
        saveSession();
        while (m_tabs->count())
            closeTab(0, false);
        m_vault->deleteLater();
        m_vault = nullptr;
        QSettings().remove(u"lastVault"_s);
    }
    for (QAction *a : std::as_const(m_vaultActions))
        a->setEnabled(false);
    rebuildWelcome();
    m_pages->setCurrentWidget(m_welcome);
    m_vaultLabel->clear();
    m_statsLabel->clear();
    setWindowTitle(u"Serat"_s);
}

void MainWindow::rebuildWelcome()
{
    while (QLayoutItem *item = m_welcomeList->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList vaults = QSettings().value(u"recentVaults"_s).toStringList();
    for (const QString &v : obsidianVaults())
        if (!vaults.contains(v))
            vaults << v;
    vaults.removeIf([](const QString &v) { return !QFileInfo(v).isDir(); });
    for (const QString &v : std::as_const(vaults)) {
        auto *b = new QPushButton;
        b->setObjectName(u"vaultRow"_s);
        b->setToolTip(tr("Open %1").arg(QDir::toNativeSeparators(v)));
        auto *name = new QLabel(QFileInfo(v).fileName());
        QFont bold = name->font();
        bold.setWeight(QFont::DemiBold);
        name->setFont(bold);
        auto *path = new QLabel;
        path->setText(path->fontMetrics().elidedText(QDir::toNativeSeparators(v), Qt::ElideMiddle, 400));
        path->setForegroundRole(QPalette::PlaceholderText);
        for (QLabel *l : {name, path})
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *rowLayout = new QVBoxLayout(b);
        rowLayout->setContentsMargins(10, 7, 10, 7);
        rowLayout->setSpacing(1);
        rowLayout->addWidget(name);
        rowLayout->addWidget(path);
        b->setMinimumHeight(rowLayout->sizeHint().height());
        connect(b, &QPushButton::clicked, this, [this, v] { openVault(v); });
        m_welcomeList->addWidget(b);
    }
}

void MainWindow::openVault(const QString &path)
{
    if (!QFileInfo(path).isDir())
        return;
    if (m_vault)
        showWelcome();

    m_vault = new Vault(path, this);
    connect(m_vault, &Vault::changed, &m_refreshTimer, qOverload<>(&QTimer::start));
    connect(m_vault, &Vault::noteChangedOnDisk, this, [this](const QString &p) {
        for (NoteView *v : views())
            if (v->path() == p)
                v->reloadFromDisk();
    });
    connect(m_vault, &Vault::renamed, this, [this](const QString &from, const QString &to) {
        for (NoteView *v : views())
            v->pathRenamed(from, to);
        for (QString &r : m_recent)
            if (r == from)
                r = to;
        m_recent.removeDuplicates();
    });

    for (QAction *a : std::as_const(m_vaultActions))
        a->setEnabled(true);
    m_tree->setVault(m_vault);
    m_search->setVault(m_vault);
    m_vaultLabel->setText(m_vault->name());
    m_pages->setCurrentWidget(m_split);

    QSettings s;
    s.setValue(u"lastVault"_s, m_vault->root());
    QStringList recentVaults = s.value(u"recentVaults"_s).toStringList();
    recentVaults.removeAll(m_vault->root());
    recentVaults.prepend(m_vault->root());
    s.setValue(u"recentVaults"_s, recentVaults.mid(0, 10));

    restoreSession();
    onVaultChanged();
    onCurrentChanged();
}

QString MainWindow::vaultKey() const
{
    return u"vault-"_s + QString::fromLatin1(QCryptographicHash::hash(m_vault->root().toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

void MainWindow::saveSession()
{
    QSettings s;
    s.setValue(u"geometry"_s, saveGeometry());
    s.setValue(u"splitter"_s, m_split->saveState());
    s.setValue(u"fontDelta"_s, m_fontDelta);
    if (!m_vault)
        return;
    QStringList tabs, modes;
    for (NoteView *v : views()) {
        tabs << v->path();
        modes << QString::number(v->mode());
    }
    s.beginGroup(vaultKey());
    s.setValue(u"tabs"_s, tabs);
    s.setValue(u"modes"_s, modes);
    s.setValue(u"current"_s, m_tabs->currentIndex());
    s.setValue(u"recent"_s, m_recent);
    s.setValue(u"leftVisible"_s, m_left->isVisible());
    s.setValue(u"rightVisible"_s, m_right->isVisible());
    s.setValue(u"expandedFolders"_s, m_tree->expandedFolders());
    s.endGroup();
}

void MainWindow::restoreSession()
{
    QSettings s;
    s.beginGroup(vaultKey());
    m_recent = s.value(u"recent"_s).toStringList();
    m_left->setVisible(s.value(u"leftVisible"_s, true).toBool());
    m_right->setVisible(s.value(u"rightVisible"_s, true).toBool());
    const QStringList tabs = s.value(u"tabs"_s).toStringList();
    const QStringList modes = s.value(u"modes"_s).toStringList();
    const int current = s.value(u"current"_s, 0).toInt();
    m_tree->setExpandedFolders(s.value(u"expandedFolders"_s).toStringList());
    s.endGroup();

    for (qsizetype i = 0; i < tabs.size(); ++i) {
        if (!m_vault->exists(tabs[i]))
            continue;
        NoteView *v = createTab();
        v->setMode(modes.value(i).toInt() == NoteView::Read ? NoteView::Read : NoteView::Edit);
        v->open(tabs[i], false);
    }
    if (m_tabs->count() == 0) {
        const QStringList &files = m_vault->files();
        const auto first = std::find_if(files.cbegin(), files.cend(), [](const QString &f) { return Vault::isNote(f); });
        if (first != files.cend())
            openPath(*first, true);
    }
    m_tabs->setCurrentIndex(qBound(0, current, m_tabs->count() - 1));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    for (NoteView *v : views())
        v->discardIfUntouched();
    saveAll();
    saveSession();
    e->accept();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_tabs->tabBar() && e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::MiddleButton) {
            const int i = m_tabs->tabBar()->tabAt(me->position().toPoint());
            if (i >= 0)
                closeTab(i);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

NoteView *MainWindow::currentView() const
{
    return qobject_cast<NoteView *>(m_tabs->currentWidget());
}

QList<NoteView *> MainWindow::views() const
{
    QList<NoteView *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *v = qobject_cast<NoteView *>(m_tabs->widget(i)))
            out << v;
    return out;
}

NoteView *MainWindow::createTab()
{
    auto *v = new NoteView(m_vault);
    v->applyFont(noteFont());
    v->setLivePreview(m_livePreview);
    v->setLinkCandidates(m_candidates);
    connect(v, &NoteView::linkRequested, this, [this, v](const QString &target, bool newTab) {
        openLink(target, v->path(), newTab);
    });
    connect(v, &NoteView::renameRequested, this, [this, v](const QString &path, const QString &name) {
        if (!validName(name)) {
            v->resetTitle();
            return;
        }
        const QString ext = Vault::isNote(path) ? u".md"_s : u'.' + QFileInfo(path).suffix();
        renamePath(path, join(dirOf(path), name + ext));
        v->resetTitle();
    });
    connect(v, &NoteView::pathChanged, this, [this, v] {
        const int i = m_tabs->indexOf(v);
        if (i < 0)
            return;
        m_tabs->setTabText(i, Vault::isNote(v->path()) ? Vault::baseName(v->path()) : v->path().section(u'/', -1));
        m_tabs->setTabToolTip(i, v->path());
        if (v == currentView())
            onCurrentChanged();
    });
    connect(v, &NoteView::statsChanged, this, [this, v] {
        if (v == currentView())
            updateStatus();
    });
    connect(v, &NoteView::moreRequested, this, [this, v](const QPoint &pos) {
        QMenu menu(this);
        fillNoteMenu(&menu, v, false);
        menu.exec(pos);
    });
    v->editor()->setCommands([this](const QString &id) { return m_commands.value(id); },
                             [this](QMenu *menu, const QString &path, bool image) { addFileActions(menu, path, image); });
    connect(v, &NoteView::hintChanged, this, [this, v](const QString &hint) {
        if (v != currentView())
            return;
        if (hint.isEmpty())
            statusBar()->clearMessage();
        else
            statusBar()->showMessage(hint);
    });
    connect(v, &NoteView::modeChanged, this, [this, v] {
        if (v == currentView())
            updateStatus();
    });
    const int index = m_tabs->addTab(v, QString());
    auto *close = new QToolButton;
    close->setIcon(Theme::icon(u"close"_s));
    close->setIconSize(QSize(14, 14));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close tab (Ctrl+W)"));
    connect(close, &QToolButton::clicked, this, [this, v] { closeTab(m_tabs->indexOf(v)); });
    m_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    return v;
}

void MainWindow::closeTab(int index, bool saveFirst)
{
    auto *v = qobject_cast<NoteView *>(m_tabs->widget(index));
    if (!v)
        return;
    if (saveFirst && !v->discardIfUntouched()) {
        v->save();
        m_closedTabs.removeAll(v->path());
        m_closedTabs << v->path();
    }
    m_tabs->removeTab(index);
    v->deleteLater();
}

void MainWindow::saveAll()
{
    for (NoteView *v : views())
        v->save();
}

void MainWindow::openPath(const QString &path, bool newTab, const QString &heading, int line)
{
    if (!m_vault || path.isEmpty())
        return;
    if (!Vault::isNote(path) && !Vault::isImage(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(path)));
        return;
    }
    NoteView *v = currentView();
    if (newTab || !v)
        v = createTab();
    v->open(path);
    m_tabs->setCurrentWidget(v);
    if (!heading.isEmpty() || line >= 0) {
        QTimer::singleShot(0, v, [v, heading, line] {
            if (!heading.isEmpty())
                v->jumpToHeading(heading);
            else
                v->jumpToLine(line);
        });
    }
    addRecent(path);
}

void MainWindow::openLink(const QString &target, const QString &from, bool newTab)
{
    if (!m_vault)
        return;
    const QString name = target.section(u'#', 0, 0).trimmed();
    const QString heading = target.section(u'#', 1).trimmed();
    if (name.isEmpty()) {
        if (NoteView *v = currentView())
            v->jumpToHeading(heading);
        return;
    }
    QString path = m_vault->resolve(name, from);
    if (path.isEmpty()) {
        if (Vault::isImage(name)) {
            QMessageBox::information(this, tr("Not found"), tr("“%1” is not in this vault.").arg(name));
            return;
        }
        // Like Obsidian's default: new notes from links go to the vault root.
        const QString rel = name.endsWith(u".md"_s, Qt::CaseInsensitive) ? name : name + u".md"_s;
        path = m_vault->createNote(rel);
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("Could not create note"), tr("Could not create “%1”.").arg(rel));
            return;
        }
    }
    openPath(path, newTab, heading);
}

bool MainWindow::validName(const QString &name)
{
    static const QString forbidden = u"\\/:*?\"<>|#^[]"_s;
    for (const QChar c : name) {
        if (forbidden.contains(c)) {
            QMessageBox::warning(this, tr("Invalid name"),
                                 tr("Names cannot contain any of these characters:\n%1").arg(forbidden));
            return false;
        }
    }
    return !name.trimmed().isEmpty() && !name.startsWith(u'.');
}

void MainWindow::newNote(const QString &dir)
{
    if (!m_vault)
        return;
    const QString path = m_vault->createNote(m_vault->uniquePath(join(dir, u"Untitled.md"_s)));
    if (path.isEmpty())
        return;
    openPath(path, false);
    if (NoteView *v = currentView()) {
        v->markNew();
        v->setMode(NoteView::Edit);
        v->focusTitle();
    }
}

void MainWindow::newFolder(const QString &dir)
{
    if (!m_vault)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name:"), QLineEdit::Normal,
                                               tr("Untitled"), &ok).trimmed();
    if (!ok || !validName(name))
        return;
    m_vault->createFolder(m_vault->uniquePath(join(dir, name)));
}

void MainWindow::promptRename(const QString &path)
{
    const bool dir = m_vault->isDir(path);
    const QFileInfo fi(path);
    const QString current = dir ? fi.fileName() : (Vault::isNote(path) ? Vault::baseName(path) : fi.completeBaseName());
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, current, &ok).trimmed();
    if (!ok || name == current || !validName(name))
        return;
    const QString ext = dir ? QString() : u'.' + fi.suffix();
    renamePath(path, join(dirOf(path), name + ext));
}

void MainWindow::renamePath(const QString &from, const QString &to)
{
    saveAll();
    const QString error = m_vault->rename(from, to);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Rename failed"), error);
        return;
    }
    for (NoteView *v : views())
        v->reloadFromDisk();
}

void MainWindow::movePath(const QString &from, const QString &toDir)
{
    renamePath(from, join(toDir, from.section(u'/', -1)));
}

void MainWindow::deletePath(const QString &path)
{
    // Respect Obsidian's "Confirm file deletion" setting.
    bool prompt = true;
    QFile app(m_vault->absolutePath(u".obsidian/app.json"_s));
    if (app.open(QIODevice::ReadOnly))
        prompt = QJsonDocument::fromJson(app.readAll()).object().value(u"promptDelete").toBool(true);
    if (prompt
        && QMessageBox::question(this, tr("Delete"), tr("Move “%1” to the trash?").arg(path)) != QMessageBox::Yes)
        return;

    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        auto *v = qobject_cast<NoteView *>(m_tabs->widget(i));
        if (v && (v->path() == path || v->path().startsWith(path + u'/')))
            closeTab(i, false);
    }
    if (!m_vault->remove(path))
        QMessageBox::warning(this, tr("Delete failed"), tr("Could not move “%1” to the trash.").arg(path));
}

void MainWindow::quickSwitcher(bool newTab)
{
    if (!m_vault)
        return;
    QuickSwitcher dlg(m_vault, m_recent, this);
    if (newTab)
        dlg.setWindowTitle(tr("Open in new tab"));
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (!dlg.createName().isEmpty())
        openLink(dlg.createName(), {}, newTab || dlg.newTab());
    else
        openPath(dlg.selectedPath(), newTab || dlg.newTab());
    if (NoteView *v = currentView())
        v->focusContent();
}

void MainWindow::commandPalette()
{
    CommandPalette dlg(menuBar(), m_recentCommands, this);
    if (dlg.exec() != QDialog::Accepted || !dlg.chosen())
        return;
    m_recentCommands.removeAll(dlg.chosenName());
    m_recentCommands.prepend(dlg.chosenName());
    m_recentCommands = m_recentCommands.mid(0, 20);
    QSettings().setValue(u"recentCommands"_s, m_recentCommands);
    // After the dialog has handed focus back to the note.
    QTimer::singleShot(0, dlg.chosen(), &QAction::trigger);
}

QWidget *MainWindow::buildFilesPage()
{
    m_files = new QWidget;
    auto *bar = new QWidget(m_files);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 6, 8, 4);
    barLayout->setSpacing(4);
    barLayout->addStretch();
    auto button = [bar, barLayout](const QString &icon, const QString &tip) {
        auto *b = new QToolButton(bar);
        b->setIcon(Theme::icon(icon));
        b->setIconSize(QSize(17, 17));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        barLayout->addWidget(b);
        return b;
    };
    connect(button(u"file-plus"_s, tr("New note")), &QToolButton::clicked, this, [this] { newNote({}); });
    connect(button(u"folder-plus"_s, tr("New folder")), &QToolButton::clicked, this, [this] { newFolder({}); });

    QToolButton *sort = button(u"sort"_s, tr("Change sort order"));
    auto *sortMenu = new QMenu(sort);
    auto *sortGroup = new QActionGroup(sortMenu);
    const std::pair<FileTree::Sort, QString> orders[] = {
        {FileTree::Sort::NameAsc, tr("File name (A to Z)")},        {FileTree::Sort::NameDesc, tr("File name (Z to A)")},
        {FileTree::Sort::ModifiedNew, tr("Modified time (new to old)")}, {FileTree::Sort::ModifiedOld, tr("Modified time (old to new)")},
        {FileTree::Sort::CreatedNew, tr("Created time (new to old)")},   {FileTree::Sort::CreatedOld, tr("Created time (old to new)")}};
    const auto saved = FileTree::Sort(QSettings().value(u"fileSort"_s, 0).toInt());
    m_tree->setSort(saved);
    for (const auto &[order, label] : orders) {
        QAction *a = sortMenu->addAction(label, this, [this, order] {
            m_tree->setSort(order);
            QSettings().setValue(u"fileSort"_s, int(order));
        });
        a->setCheckable(true);
        a->setChecked(order == saved);
        sortGroup->addAction(a);
        if (order == FileTree::Sort::NameDesc || order == FileTree::Sort::ModifiedOld)
            sortMenu->addSeparator();
    }
    connect(sort, &QToolButton::clicked, this, [sort, sortMenu] { sortMenu->exec(sort->mapToGlobal(QPoint(0, sort->height()))); });

    QToolButton *reveal = button(u"reveal"_s, tr("Auto-reveal current file"));
    reveal->setCheckable(true);
    m_autoReveal = QSettings().value(u"autoReveal"_s, false).toBool();
    reveal->setChecked(m_autoReveal);
    connect(reveal, &QToolButton::toggled, this, [this](bool on) {
        m_autoReveal = on;
        QSettings().setValue(u"autoReveal"_s, on);
        if (NoteView *v = currentView(); on && v)
            m_tree->reveal(v->path());
    });

    QToolButton *collapse = button(u"collapse"_s, tr("Collapse all"));
    auto syncCollapse = [this, collapse] {
        const bool expanded = m_tree->anyExpanded();
        collapse->setIcon(Theme::icon(expanded ? u"collapse"_s : u"expand"_s));
        collapse->setToolTip(expanded ? tr("Collapse all") : tr("Expand all"));
    };
    connect(collapse, &QToolButton::clicked, this, [this, syncCollapse] {
        if (m_tree->anyExpanded())
            m_tree->collapseAll();
        else
            m_tree->expandAll();
        syncCollapse();
    });
    connect(m_tree, &QTreeView::expanded, this, syncCollapse);
    connect(m_tree, &QTreeView::collapsed, this, syncCollapse);
    barLayout->addStretch();

    auto *layout = new QVBoxLayout(m_files);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(bar);
    layout->addWidget(m_tree, 1);
    return m_files;
}

void MainWindow::searchVault(const QString &text)
{
    m_left->show();
    m_left->setCurrentWidget(m_search);
    if (!text.isEmpty())
        m_search->search(text);
    m_search->focusSearch();
}

void MainWindow::showTabMenu(const QPoint &pos)
{
    QTabBar *bar = m_tabs->tabBar();
    auto *v = qobject_cast<NoteView *>(m_tabs->widget(bar->tabAt(pos)));
    if (!v || !m_vault)
        return;
    QMenu menu(this);
    fillNoteMenu(&menu, v, true);
    menu.exec(bar->mapToGlobal(pos));
}

// The tab menu and the note's "More options" menu, in Obsidian's order.
void MainWindow::fillNoteMenu(QMenu *menu, NoteView *v, bool tab)
{
    const QString path = v->path();
    if (tab) {
        menu->addAction(tr("Close"), this, [this, v] { closeTab(m_tabs->indexOf(v)); });
        menu->addAction(tr("Close others"), this, [this, v] {
            for (int i = m_tabs->count() - 1; i >= 0; --i)
                if (m_tabs->widget(i) != v)
                    closeTab(i);
        })->setEnabled(m_tabs->count() > 1);
        menu->addAction(tr("Close tabs to the right"), this, [this, v] {
            for (int i = m_tabs->count() - 1; i > m_tabs->indexOf(v); --i)
                closeTab(i);
        })->setEnabled(m_tabs->indexOf(v) < m_tabs->count() - 1);
        menu->addSeparator();
    }
    if (v->isNote()) {
        const bool reading = v->mode() == NoteView::Read;
        menu->addAction(reading ? tr("Editing view") : tr("Reading view"), this,
                        [v, reading] { v->setMode(reading ? NoteView::Edit : NoteView::Read); });
        QAction *source = menu->addAction(tr("Source mode"), this, [this] { m_commands.value(u"view.live"_s)->trigger(); });
        source->setCheckable(true);
        source->setChecked(!m_livePreview);
        menu->addSeparator();
    }
    menu->addAction(tr("Rename..."), this, [v] { v->focusTitle(); });
    menu->addAction(tr("Move file to..."), this, [this, path] { chooseFolderAndMove(path); });
    if (v->isNote()) {
        menu->addAction(m_commands.value(u"insert.property"_s));
        menu->addSeparator();
        menu->addAction(m_commands.value(u"edit.find"_s));
        menu->addAction(m_commands.value(u"edit.replace"_s));
    }
    menu->addSeparator();
    PathActions::addCopyMenu(menu, *m_vault, path);
    menu->addSeparator();
    menu->addAction(tr("Open in default app"), this, [this, path] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(path))); });
    menu->addAction(tr("Show in system explorer"), this, [this, path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_vault->absolutePath(path)).absolutePath()));
    });
    menu->addAction(tr("Reveal file in navigation"), this, [this, path] { revealInTree(path); });
    menu->addSeparator();
    menu->addAction(tr("Delete file"), this, [this, path] { deletePath(path); });
}

void MainWindow::addFileActions(QMenu *menu, const QString &path, bool deletable)
{
    PathActions::addCopyMenu(menu, *m_vault, path);
    menu->addSeparator();
    menu->addAction(tr("Rename..."), this, [this, path] { promptRename(path); });
    menu->addAction(tr("Move file to..."), this, [this, path] { chooseFolderAndMove(path); });
    menu->addSeparator();
    menu->addAction(tr("Open in default app"), this, [this, path] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(path))); });
    menu->addAction(tr("Show in system explorer"), this, [this, path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_vault->absolutePath(path)).absolutePath()));
    });
    menu->addAction(tr("Reveal file in navigation"), this, [this, path] { revealInTree(path); });
    if (deletable) {
        menu->addSeparator();
        menu->addAction(Vault::isImage(path) ? tr("Delete image") : tr("Delete file"), this, [this, path] { deletePath(path); });
    }
}

void MainWindow::revealInTree(const QString &path)
{
    m_left->show();
    m_left->setCurrentWidget(m_files);
    m_tree->reveal(path);
    m_tree->setFocus();
}

void MainWindow::duplicatePath(const QString &path)
{
    const QString copy = m_vault->uniquePath(path);
    if (m_vault->isDir(path)) {
        const QDir from(m_vault->absolutePath(path));
        bool ok = QDir().mkpath(m_vault->absolutePath(copy));
        QDirIterator it(from.path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (ok && it.hasNext()) {
            const QFileInfo fi(it.next());
            const QString target = m_vault->absolutePath(copy) + u'/' + from.relativeFilePath(fi.absoluteFilePath());
            ok = fi.isDir() ? QDir().mkpath(target) : QFile::copy(fi.absoluteFilePath(), target);
        }
        if (!ok)
            QMessageBox::warning(this, tr("Copy failed"), tr("Could not copy every file in “%1”.").arg(path));
        m_vault->rescan();
        m_tree->reveal(copy);
        return;
    }
    if (!QFile::copy(m_vault->absolutePath(path), m_vault->absolutePath(copy))) {
        QMessageBox::warning(this, tr("Duplicate failed"), tr("Could not copy “%1”.").arg(path));
        return;
    }
    m_vault->rescan();
    openPath(copy, false);
    if (NoteView *v = currentView(); v && v->isNote())
        v->focusTitle();
}

void MainWindow::chooseFolderAndMove(const QString &path)
{
    const QString from = dirOf(path);
    const QString root = tr("(vault root)");
    QStringList folders;
    if (!from.isEmpty())
        folders << root;
    QDirIterator it(m_vault->root(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = m_vault->relativePath(it.next());
        const bool hidden = dir.startsWith(u'.') || dir.contains(u"/."_s);
        if (!hidden && dir != from && dir != path && !dir.startsWith(path + u'/'))
            folders << dir;
    }
    std::sort(folders.begin() + (from.isEmpty() ? 0 : 1), folders.end());
    if (folders.isEmpty()) {
        QMessageBox::information(this, tr("Move to"), tr("There is no other folder to move “%1” to.").arg(path));
        return;
    }
    bool ok = false;
    const QString to = QInputDialog::getItem(this, tr("Move to"), tr("Move “%1” to:").arg(path.section(u'/', -1)), folders,
                                             0, false, &ok);
    if (ok)
        movePath(path, to == root ? QString() : to);
}

QString MainWindow::findVault(const QString &name) const
{
    QStringList candidates;
    if (m_vault)
        candidates << m_vault->root();
    candidates << QSettings().value(u"recentVaults"_s).toStringList() << obsidianVaults();
    for (const QString &dir : std::as_const(candidates))
        if (QFileInfo(dir).fileName() == name && QFileInfo(dir).isDir())
            return dir;
    return {};
}

void MainWindow::handleArgument(const QString &arg)
{
    if (arg.startsWith(u"serat:"_s, Qt::CaseInsensitive))
        openUrl(QUrl(arg));
    else if (const QString dir = arg.startsWith(u"file:"_s) ? QUrl(arg).toLocalFile() : arg; QFileInfo(dir).isDir())
        openVault(QFileInfo(dir).absoluteFilePath());
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::openUrl(const QUrl &url)
{
    const QUrlQuery query(url);
    const QString name = query.queryItemValue(u"vault"_s, QUrl::FullyDecoded);
    const QString file = query.queryItemValue(u"file"_s, QUrl::FullyDecoded);
    if (url.host() != u"open" || name.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot open link"), tr("“%1” is not a Serat link.").arg(url.toDisplayString()));
        return;
    }
    const QString root = findVault(name);
    if (root.isEmpty()) {
        QMessageBox::warning(this, tr("Vault not found"),
                             tr("No vault named “%1” is known here. Open its folder in Serat once, then try the link again.").arg(name));
        return;
    }
    if (!m_vault || m_vault->root() != root)
        openVault(root);
    if (file.isEmpty())
        return;
    QString path = file;
    if (!m_vault->exists(path) && m_vault->exists(path + u".md"_s))
        path += u".md"_s;
    if (!m_vault->exists(path)) {
        QMessageBox::warning(this, tr("Note not found"), tr("“%1” is not in the vault “%2”.").arg(file, name));
        return;
    }
    NoteView *open = nullptr;
    for (NoteView *v : views())
        if (v->path() == path)
            open = v;
    if (open)
        m_tabs->setCurrentWidget(open);
    else
        openPath(path, true);
    if (NoteView *v = currentView())
        v->focusContent();
}

void MainWindow::zoom(double delta)
{
    m_fontDelta = qBound(-6.0, m_fontDelta + delta, 16.0);
    const QFont f = noteFont();
    for (NoteView *v : views())
        v->applyFont(f);
}

QFont MainWindow::noteFont() const
{
    QFont f = QApplication::font();
    f.setPointSizeF(qMax(12.0, f.pointSizeF() + 1.5) + m_fontDelta);
    return f;
}

void MainWindow::addRecent(const QString &path)
{
    m_recent.removeAll(path);
    m_recent.prepend(path);
    if (m_recent.size() > 30)
        m_recent.resize(30);
}

void MainWindow::onCurrentChanged()
{
    refreshPanels();
    updateStatus();
    updateTitle();
    if (NoteView *v = currentView(); v && m_autoReveal)
        m_tree->reveal(v->path());
}

void MainWindow::onVaultChanged()
{
    if (!m_vault)
        return;
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        auto *v = qobject_cast<NoteView *>(m_tabs->widget(i));
        if (v && !m_vault->exists(v->path()))
            closeTab(i, false);
    }
    m_candidates.clear();
    for (const QString &f : m_vault->files())
        m_candidates << m_vault->linkText(f);
    for (NoteView *v : views()) {
        v->setLinkCandidates(m_candidates);
        v->refreshRender();
    }
    m_search->rerun();
    refreshPanels();
}

void MainWindow::refreshPanels()
{
    NoteView *v = currentView();
    const QString path = v ? v->path() : QString();
    m_backlinks->showFor(m_vault, path);
    m_outgoing->showFor(m_vault, path);
    m_outline->showFor(m_vault ? m_vault->note(path) : nullptr);
}

void MainWindow::updateStatus()
{
    NoteView *v = currentView();
    m_statsLabel->setText(v ? v->statsText() : QString());
}

void MainWindow::updateTitle()
{
    NoteView *v = currentView();
    QStringList parts;
    if (v && !v->path().isEmpty())
        parts << Vault::baseName(v->path());
    if (m_vault)
        parts << m_vault->name();
    parts << u"Serat"_s;
    setWindowTitle(parts.join(u" - "_s));
}
