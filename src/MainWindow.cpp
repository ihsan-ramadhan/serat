#include "MainWindow.h"

#include "CommandPalette.h"
#include "Editor.h"
#include "Export.h"
#include "Graph.h"
#include "Markdown.h"
#include "NoteView.h"
#include "Panels.h"
#include "PathActions.h"
#include "QuickSwitcher.h"
#include "Reader.h"
#include "RecoveryDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "Spelling.h"
#include "Theme.h"
#include "Vault.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDrag>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
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

#include <numeric>

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
    // Narrow sidebars squeeze the icon tabs instead of scrolling them.
    QSize minimumTabSizeHint(int index) const override
    {
        return QSize(iconSize().width() + 14, QTabBar::minimumTabSizeHint(index).height());
    }
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
    PanelTabs()
    {
        setTabBar(new IconTabBar);
        setUsesScrollButtons(false);
    }
};

constexpr auto tabMime = "application/x-serat-tab";

// Covers a pane while a tab is dragged: dropping in the middle moves the tab there, near an edge splits the pane.
class DropZone : public QWidget
{
public:
    enum Zone { None, Center, Left, Right, Top, Bottom };
    std::function<void(const QByteArray &, Zone)> dropped;

    explicit DropZone(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(u"dropZone"_s);
        setAcceptDrops(true);
        hide();
    }

    Zone zoneAt(const QPointF &p) const
    {
        const double fx = p.x() / qMax(1, width()), fy = p.y() / qMax(1, height());
        const std::pair<double, Zone> edges[] = {{fx, Left}, {1 - fx, Right}, {fy, Top}, {1 - fy, Bottom}};
        const auto nearest = *std::min_element(std::begin(edges), std::end(edges));
        return nearest.first < 0.25 ? nearest.second : Center;
    }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (!e->mimeData()->hasFormat(QString::fromLatin1(tabMime)))
            return e->ignore();
        e->acceptProposedAction();
        show(zoneAt(e->position()));
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        e->acceptProposedAction();
        show(zoneAt(e->position()));
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { show(None); }
    void dropEvent(QDropEvent *e) override
    {
        const Zone z = zoneAt(e->position());
        show(None);
        e->acceptProposedAction();
        if (dropped)
            dropped(e->mimeData()->data(QString::fromLatin1(tabMime)), z);
    }
    void paintEvent(QPaintEvent *) override
    {
        if (m_zone == None)
            return;
        QRect r = rect();
        if (m_zone == Left || m_zone == Right)
            r.setWidth(width() / 2);
        if (m_zone == Top || m_zone == Bottom)
            r.setHeight(height() / 2);
        if (m_zone == Right)
            r.moveRight(rect().right());
        if (m_zone == Bottom)
            r.moveBottom(rect().bottom());
        QPainter p(this);
        QColor c = palette().color(QPalette::Highlight);
        c.setAlpha(40);
        p.fillRect(r.adjusted(4, 4, -4, -4), c);
        c.setAlpha(160);
        p.setPen(QPen(c, 2));
        p.drawRect(r.adjusted(5, 5, -5, -5));
    }

private:
    using QWidget::show;
    void show(Zone z)
    {
        if (z != m_zone) {
            m_zone = z;
            update();
        }
    }
    Zone m_zone = None;
};

QSplitter *newSplitter(Qt::Orientation orientation)
{
    auto *s = new QSplitter(orientation);
    s->setChildrenCollapsible(false);
    s->setHandleWidth(1);
    return s;
}

// Moves the panes of `inner` into `outer` where `inner` was, keeping their share of the space.
void splice(QSplitter *outer, QSplitter *inner)
{
    const int at = outer->indexOf(inner);
    QList<int> sizes = outer->sizes();
    const QList<int> innerSizes = inner->sizes();
    const int total = qMax(1, std::accumulate(innerSizes.cbegin(), innerSizes.cend(), 0));
    const int share = sizes.takeAt(at);
    for (int k = 0; k < innerSizes.size(); ++k)
        sizes.insert(at + k, share * innerSizes[k] / total);
    for (int k = 0; inner->count(); ++k)
        outer->insertWidget(at + k, inner->widget(0));
    inner->setParent(nullptr);
    inner->deleteLater();
    outer->setSizes(sizes);
}

// The layout of the panes: a pane's index, or a splitter with its direction, sizes and children.
QVariant paneLayout(QWidget *w, const QList<QTabWidget *> &panes)
{
    if (auto *pane = qobject_cast<QTabWidget *>(w))
        return panes.indexOf(pane);
    auto *s = qobject_cast<QSplitter *>(w);
    QVariantList children, sizes;
    for (int i = 0; i < s->count(); ++i) {
        children << paneLayout(s->widget(i), panes);
        sizes << s->sizes().value(i);
    }
    return QVariantMap{{u"vertical"_s, s->orientation() == Qt::Vertical}, {u"sizes"_s, sizes}, {u"children"_s, children}};
}

QWidget *buildLayout(const QVariant &node, const QList<QTabWidget *> &panes)
{
    if (node.typeId() != QMetaType::QVariantMap)
        return panes.value(node.toInt(), nullptr);
    const QVariantMap map = node.toMap();
    QList<QWidget *> children;
    QList<int> sizes;
    const QVariantList saved = map.value(u"children"_s).toList(), savedSizes = map.value(u"sizes"_s).toList();
    for (int i = 0; i < saved.size(); ++i) {
        if (QWidget *w = buildLayout(saved[i], panes)) {
            children << w;
            sizes << qMax(50, savedSizes.value(i).toInt());
        }
    }
    if (children.size() < 2)
        return children.value(0);
    QSplitter *s = newSplitter(map.value(u"vertical"_s).toBool() ? Qt::Vertical : Qt::Horizontal);
    for (QWidget *w : children)
        s->addWidget(w);
    s->setSizes(sizes);
    return s;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    buildActions();
    Settings::applyShortcuts(m_commands.values());
    // Loading the dictionaries takes a moment; the window shows first.
    QTimer::singleShot(0, this, [] { Spelling::instance().reload(); });
    setReadableLineLength(Settings::app(u"readableWidth"_s, true).toBool());

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
    m_bookmarks = new BookmarksPanel;
    m_left = panelTabs({{buildFilesPage(), u"folder", tr("Files")},
                        {m_search, u"search", tr("Search (Ctrl+Shift+F)")},
                        {m_bookmarks, u"bookmark", tr("Bookmarks")}});

    m_panes = newSplitter(Qt::Horizontal);
    m_tabs = createPane();
    m_panes->addWidget(m_tabs);

    m_backlinks = new BacklinksPanel;
    m_outgoing = new OutgoingPanel;
    m_outline = new OutlinePanel;
    m_tags = new TagsPanel;
    m_localGraph = new GraphView;
    m_localGraph->setObjectName(u"localGraph"_s);
    connect(m_localGraph, &GraphView::openRequested, this, [this](const QString &p, bool newTab) { openPath(p, newTab); });
    m_right = panelTabs({{m_backlinks, u"backlinks", tr("Backlinks")},
                         {m_outgoing, u"outgoing", tr("Outgoing links")},
                         {m_localGraph, u"graph", tr("Local graph")},
                         {m_tags, u"tag", tr("Tags")},
                         {m_outline, u"outline", tr("Outline")}});
    m_preview = new PreviewPopup(this);
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(300);
    connect(&m_previewTimer, &QTimer::timeout, this, &MainWindow::showPreview);

    m_split = new QSplitter;
    m_split->addWidget(m_left);
    m_empty = new QWidget;
    m_empty->setObjectName(u"emptyState"_s);
    auto *emptyLayout = new QVBoxLayout(m_empty);
    auto *emptyTitle = new QLabel(tr("No note is open"));
    emptyTitle->setObjectName(u"emptyTitle"_s);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyTitle, 0, Qt::AlignHCenter);
    emptyLayout->addSpacing(10);
    auto emptyAction = [this, emptyLayout](const QString &text, auto fn) {
        auto *b = new QPushButton(text);
        b->setObjectName(u"emptyAction"_s);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, fn);
        emptyLayout->addWidget(b, 0, Qt::AlignHCenter);
    };
    emptyAction(tr("Create new note (Ctrl+N)"), [this] { newNote(defaultNoteDir()); });
    emptyAction(tr("Go to file (Ctrl+O)"), [this] { quickSwitcher(); });
    emptyAction(tr("Open today's daily note"), [this] { openDailyNote(); });
    emptyLayout->addStretch(2);
    m_center = new QStackedWidget;
    m_center->addWidget(m_panes);
    m_center->addWidget(m_empty);
    m_split->addWidget(m_center);
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
    m_backlinkCount = new QToolButton;
    m_backlinkCount->setObjectName(u"statusButton"_s);
    m_backlinkCount->setToolTip(tr("Show backlinks"));
    m_backlinkCount->setFocusPolicy(Qt::NoFocus);
    connect(m_backlinkCount, &QToolButton::clicked, this, [this] {
        m_right->show();
        m_right->setCurrentWidget(m_backlinks);
    });
    statusBar()->addPermanentWidget(m_backlinkCount);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QTabWidget *pane : panes())
            if (now && pane != m_tabs && pane->isAncestorOf(now))
                setActivePane(pane);
    });

    connect(m_tree, &FileTree::openRequested, this, [this](const QString &p, bool newTab) { openPath(p, newTab); });
    connect(m_tree, &FileTree::newNoteRequested, this, &MainWindow::newNote);
    connect(m_tree, &FileTree::newFolderRequested, this, &MainWindow::newFolder);
    connect(m_tree, &FileTree::renameRequested, this, &MainWindow::promptRename);
    connect(m_tree, &FileTree::deleteRequested, this, &MainWindow::deletePath);
    connect(m_tree, &FileTree::moveRequested, this, &MainWindow::movePath);
    connect(m_tree, &FileTree::moveToRequested, this, [this](const QString &path) { chooseFolderAndMove({path}); });
    connect(m_tree, &FileTree::moveManyRequested, this, &MainWindow::chooseFolderAndMove);
    connect(m_tree, &FileTree::deleteManyRequested, this, &MainWindow::deletePaths);
    m_tree->extraActions = [this](QMenu *menu, const QString &path) {
        menu->addAction(m_bookmarks->has(path) ? tr("Remove bookmark") : tr("Bookmark"), this, [this, path] { m_bookmarks->toggle(path); });
    };
    connect(m_bookmarks, &BookmarksPanel::openRequested, this, [this](const QString &path, bool newTab) {
        if (m_vault && m_vault->isDir(path))
            revealInTree(path);
        else
            openPath(path, newTab);
    });
    connect(m_bookmarks, &BookmarksPanel::searchRequested, this, [this](const QString &q) { searchVault(q); });
    connect(m_search, &SearchPanel::bookmarkRequested, this, [this](const QString &q) {
        m_bookmarks->addSearch(q);
        statusBar()->showMessage(tr("Search bookmarked"), 3000);
    });
    connect(m_tags, &TagsPanel::searchRequested, this, [this](const QString &q) { searchVault(q); });
    connect(m_tree, &FileTree::duplicateRequested, this, &MainWindow::duplicatePath);
    connect(m_tree, &FileTree::searchInFolderRequested, this, [this](const QString &dir) { searchVault(u"path:\""_s + dir + u"/\" "_s); });
    connect(m_search, &SearchPanel::openRequested, this,
            [this](const QString &p, int line, bool newTab) { openPath(p, newTab, {}, line); });
    connect(m_backlinks, &BacklinksPanel::linkMentionRequested, this, &MainWindow::linkMention);
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
        a->setObjectName(id);
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
    add(file, u"file.new"_s, tr("New note"), QKeySequence::New, [this] { newNote(defaultNoteDir()); });
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
        withView([this](NoteView *v) { chooseFolderAndMove({v->path()}); }));
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
    add(file, u"file.bookmark"_s, tr("Bookmark current file"), {}, withView([this](NoteView *v) { m_bookmarks->toggle(v->path()); }));
    add(file, u"file.recovery"_s, tr("File recovery…"), {}, [this] { openRecovery(currentView() ? currentView()->path() : QString()); });
    add(file, u"file.delete"_s, tr("Delete current file"), {}, withView([this](NoteView *v) { deletePath(v->path()); }));
    file->addSeparator();
    add(file, u"file.exportPdf"_s, tr("Export to PDF…"), {}, withView([this](NoteView *v) { exportNote(v, u"pdf"_s); }));
    add(file, u"file.exportHtml"_s, tr("Export to HTML…"), {}, withView([this](NoteView *v) { exportNote(v, u"html"_s); }));
    add(file, u"file.print"_s, tr("Print…"), {}, withView([this](NoteView *v) { exportNote(v, u"print"_s); }));
    file->addSeparator();
    add(file, u"file.save"_s, tr("Save"), QKeySequence::Save, [this] { saveAll(); });
    add(file, u"app.settings"_s, tr("Settings…"), QKeySequence(u"Ctrl+,"_s), [this] { openSettings(); }, false);
    add(file, u"tab.close"_s, tr("Close tab"), QKeySequence(u"Ctrl+W"_s), [this] {
        if (QWidget *page = m_tabs->currentWidget())
            closeTab(page);
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
    add(insert, u"insert.template"_s, tr("Insert template…"), {}, withEditor([this](Editor *e) { insertTemplate(e); }));
    add(insert, u"insert.date"_s, tr("Insert current date"), {}, withEditor([this](Editor *e) {
        e->insertPlainText(Settings::formatMoment(QDateTime::currentDateTime(), Settings::templates(*m_vault).dateFormat));
    }));
    add(insert, u"insert.time"_s, tr("Insert current time"), {}, withEditor([this](Editor *e) {
        e->insertPlainText(Settings::formatMoment(QDateTime::currentDateTime(), Settings::templates(*m_vault).timeFormat));
    }));
    insert->addSeparator();
    add(insert, u"insert.property"_s, tr("Add file property"), QKeySequence(u"Ctrl+;"_s), [this] {
        // Unlike the other editor commands this one also runs from inside the properties panel.
        if (NoteView *v = currentView(); v && v->isNote() && v->mode() == NoteView::Edit)
            v->editor()->addProperty();
    });

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    add(go, u"daily.today"_s, tr("Open today's daily note"), {}, [this] { openDailyNote(); });
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
    add(view, u"fold.toggle"_s, tr("Toggle fold"), QKeySequence(u"Ctrl+Shift+["_s), withEditor([](Editor *e) { e->toggleFoldAtCursor(); }));
    add(view, u"fold.all"_s, tr("Fold all headings and lists"), {}, withEditor([](Editor *e) { e->foldAll(true); }));
    add(view, u"fold.none"_s, tr("Unfold all headings and lists"), {}, withEditor([](Editor *e) { e->foldAll(false); }));
    view->addSeparator();
    add(view, u"pane.splitRight"_s, tr("Split right"), {}, [this] { split(Qt::Horizontal); });
    add(view, u"pane.splitDown"_s, tr("Split down"), {}, [this] { split(Qt::Vertical); });
    add(view, u"view.graph"_s, tr("Open graph view"), QKeySequence(u"Ctrl+G"_s), [this] { openGraph(); });
    add(view, u"tab.pin"_s, tr("Pin or unpin tab"), {}, withView([this](NoteView *v) {
        v->setPinned(!v->isPinned());
        updateTabButton(v);
    }));
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
        for (NoteView *v : views())
            closeView(v, false);
        for (QWidget *page : tabPages())
            closeTab(page);
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
    m_bookmarks->setVault(m_vault, vaultKey());
    m_vaultLabel->setText(m_vault->name());
    m_pages->setCurrentWidget(m_split);

    QSettings s;
    s.setValue(u"lastVault"_s, m_vault->root());
    QStringList recentVaults = s.value(u"recentVaults"_s).toStringList();
    recentVaults.removeAll(m_vault->root());
    recentVaults.prepend(m_vault->root());
    s.setValue(u"recentVaults"_s, recentVaults.mid(0, 10));

    m_vault->recovery().prune();
    restoreSession();
    if (Settings::vault(*m_vault, u"dailyOnStartup"_s, {}, {}, false).toBool())
        openDailyNote();
    onVaultChanged();
    onCurrentChanged();
}

QString MainWindow::vaultKey() const
{
    return Settings::vaultGroup(*m_vault);
}

void MainWindow::saveSession()
{
    QSettings s;
    s.setValue(u"geometry"_s, saveGeometry());
    s.setValue(u"splitter"_s, m_split->saveState());
    s.setValue(u"fontDelta"_s, m_fontDelta);
    if (!m_vault)
        return;
    QVariantList saved;
    for (QTabWidget *pane : panes()) {
        QStringList tabs, modes, pinned;
        for (int i = 0; i < pane->count(); ++i) {
            auto *v = qobject_cast<NoteView *>(pane->widget(i));
            if (!v) {
                tabs << QString();
                modes << u"graph"_s;
                continue;
            }
            tabs << v->path();
            modes << QString::number(v->mode());
            if (v->isPinned())
                pinned << v->path();
        }
        saved << QVariantMap{{u"tabs"_s, tabs}, {u"modes"_s, modes}, {u"pinned"_s, pinned}, {u"current"_s, pane->currentIndex()}};
    }
    s.beginGroup(vaultKey());
    s.remove(u"tabs"_s);
    s.remove(u"modes"_s);
    s.remove(u"current"_s);
    s.setValue(u"panes"_s, saved);
    s.setValue(u"layout"_s, paneLayout(m_panes, panes()));
    s.setValue(u"activePane"_s, panes().indexOf(m_tabs));
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
    QVariantList saved = s.value(u"panes"_s).toList();
    if (saved.isEmpty() && s.contains(u"tabs"_s))
        saved << QVariantMap{{u"tabs"_s, s.value(u"tabs"_s)}, {u"modes"_s, s.value(u"modes"_s)}, {u"current"_s, s.value(u"current"_s)}};
    const int active = s.value(u"activePane"_s, 0).toInt();
    const QVariant layout = s.value(u"layout"_s);
    m_tree->setExpandedFolders(s.value(u"expandedFolders"_s).toStringList());
    s.endGroup();

    QTabWidget *activePane = nullptr;
    QList<QTabWidget *> made(saved.size(), nullptr);
    for (int p = 0; p < saved.size(); ++p) {
        const QVariantMap pane = saved[p].toMap();
        const QStringList tabs = pane.value(u"tabs"_s).toStringList();
        const QStringList modes = pane.value(u"modes"_s).toStringList();
        const QStringList pinned = pane.value(u"pinned"_s).toStringList();
        if (std::none_of(tabs.cbegin(), tabs.cend(), [this](const QString &t) { return m_vault->exists(t); })
            && !modes.contains(u"graph"_s))
            continue;
        if (m_tabs->count()) {
            m_tabs = createPane();
            m_panes->addWidget(m_tabs);
        }
        made[p] = m_tabs;
        for (qsizetype i = 0; i < tabs.size(); ++i) {
            if (modes.value(i) == u"graph") {
                createGraphTab();
                continue;
            }
            if (!m_vault->exists(tabs[i]))
                continue;
            NoteView *v = createTab();
            v->setMode(modes.value(i).toInt() == NoteView::Read ? NoteView::Read : NoteView::Edit);
            v->open(tabs[i], false);
            if (pinned.contains(tabs[i])) {
                v->setPinned(true);
                updateTabButton(v);
            }
        }
        m_tabs->setCurrentIndex(qBound(0, pane.value(u"current"_s).toInt(), m_tabs->count() - 1));
        if (p == active)
            activePane = m_tabs;
    }
    if (auto *root = qobject_cast<QSplitter *>(buildLayout(layout, made))) {
        m_panes->setOrientation(root->orientation());
        m_panes->addWidget(root);
        splice(m_panes, root);
    }
    m_tabs = activePane ? activePane : panes().first();
    onCurrentChanged();
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
    auto *bar = qobject_cast<QTabBar *>(obj);
    QTabWidget *pane = bar ? qobject_cast<QTabWidget *>(bar->parentWidget()) : nullptr;
    if (pane && e->type() == QEvent::MouseButtonPress) {
        setActivePane(pane);
        m_tabPress = static_cast<QMouseEvent *>(e)->position().toPoint();
    }
    // A tab dragged off its bar can go to another pane.
    if (pane && e->type() == QEvent::MouseMove) {
        auto *me = static_cast<QMouseEvent *>(e);
        const QPoint pos = me->position().toPoint();
        if ((me->buttons() & Qt::LeftButton) && (pos - m_tabPress).manhattanLength() > QApplication::startDragDistance()
            && !bar->rect().adjusted(0, -24, 0, 24).contains(pos)) {
            startTabDrag(pane);
            return true;
        }
    }
    if (pane && e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::MiddleButton) {
            if (QWidget *page = pane->widget(bar->tabAt(me->position().toPoint())))
                closeTab(page);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

QTabWidget *MainWindow::createPane()
{
    auto *pane = new QTabWidget;
    pane->setDocumentMode(true);
    pane->tabBar()->setObjectName(u"notes"_s);
    pane->tabBar()->setDrawBase(false);
    pane->setMovable(true);
    pane->setElideMode(Qt::ElideRight);
    pane->setUsesScrollButtons(true);
    pane->tabBar()->setExpanding(false);
    pane->tabBar()->installEventFilter(this);
    pane->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pane->tabBar(), &QWidget::customContextMenuRequested, this, [this, pane](const QPoint &pos) {
        setActivePane(pane);
        showTabMenu(pane, pos);
    });
    auto *plus = new QToolButton;
    plus->setIcon(Theme::icon(u"file-plus"_s));
    plus->setIconSize(QSize(18, 18));
    plus->setAutoRaise(true);
    plus->setToolTip(tr("New note (Ctrl+N)"));
    auto *corner = new QWidget;
    auto *cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 6, 4); // aligns with the tab titles
    cornerLayout->addWidget(plus, 0, Qt::AlignVCenter);
    pane->setCornerWidget(corner, Qt::TopRightCorner);
    connect(plus, &QToolButton::clicked, this, [this, pane] {
        setActivePane(pane);
        newNote(defaultNoteDir());
    });
    connect(pane, &QTabWidget::currentChanged, this, [this, pane] {
        if (pane == m_tabs)
            onCurrentChanged();
    });
    auto *zone = new DropZone(pane);
    zone->dropped = [this, pane](const QByteArray &data, DropZone::Zone z) {
        for (QWidget *page : tabPages())
            if (QByteArray::number(quintptr(page)) == data)
                return moveTab(page, pane, z);
    };
    return pane;
}

QList<QTabWidget *> MainWindow::panes() const
{
    QList<QTabWidget *> out;
    std::function<void(QSplitter *)> walk = [&](QSplitter *s) {
        for (int i = 0; i < s->count(); ++i) {
            if (auto *pane = qobject_cast<QTabWidget *>(s->widget(i)))
                out << pane;
            else if (auto *inner = qobject_cast<QSplitter *>(s->widget(i)))
                walk(inner);
        }
    };
    walk(m_panes);
    return out;
}

QTabWidget *MainWindow::paneOf(QWidget *page) const
{
    for (QTabWidget *pane : panes())
        if (pane->indexOf(page) >= 0)
            return pane;
    return nullptr;
}

void MainWindow::setActivePane(QTabWidget *pane)
{
    if (pane == m_tabs)
        return;
    m_tabs = pane;
    onCurrentChanged();
}

// Opens the current note again in a new pane to the right or below.
void MainWindow::split(Qt::Orientation orientation)
{
    NoteView *from = currentView();
    m_tabs = insertPane(m_tabs, orientation, true);
    NoteView *v = createTab();
    if (from)
        v->open(from->path(), false);
    onCurrentChanged();
    v->focusContent();
}

// A new, empty pane next to `beside`, taking half of its space.
QTabWidget *MainWindow::insertPane(QTabWidget *beside, Qt::Orientation orientation, bool after)
{
    auto *parent = qobject_cast<QSplitter *>(beside->parentWidget());
    QTabWidget *pane = createPane();
    if (parent->count() == 1)
        parent->setOrientation(orientation);
    const int at = parent->indexOf(beside);
    QList<int> sizes = parent->sizes();
    if (parent->orientation() == orientation) {
        const int half = sizes[at] / 2;
        sizes[at] -= half;
        sizes.insert(after ? at + 1 : at, half);
        parent->insertWidget(after ? at + 1 : at, pane);
    } else {
        QSplitter *inner = newSplitter(orientation);
        parent->replaceWidget(at, inner);
        inner->addWidget(after ? beside : pane);
        inner->addWidget(after ? pane : beside);
        beside->show();
        inner->setSizes({1000, 1000});
    }
    parent->setSizes(sizes);
    return pane;
}

void MainWindow::dropEmptyPane(QTabWidget *pane)
{
    if (pane->count() || panes().size() < 2)
        return;
    auto *parent = qobject_cast<QSplitter *>(pane->parentWidget());
    pane->hide();
    pane->setParent(nullptr);
    pane->deleteLater();
    tidyPanes(parent);
    if (pane == m_tabs) {
        m_tabs = panes().first();
        onCurrentChanged();
    }
}

// Splitters left with one pane give their place to it, so closing panes undoes the splits.
void MainWindow::tidyPanes(QSplitter *s)
{
    while (s != m_panes && s->count() < 2) {
        auto *parent = qobject_cast<QSplitter *>(s->parentWidget());
        if (QWidget *only = s->widget(0)) {
            const QList<int> sizes = parent->sizes();
            parent->replaceWidget(parent->indexOf(s), only);
            only->show();
            parent->setSizes(sizes);
            if (auto *inner = qobject_cast<QSplitter *>(only); inner && inner->orientation() == parent->orientation())
                splice(parent, inner);
        } else {
            s->setParent(nullptr);
        }
        s->deleteLater();
        s = parent;
    }
    if (s == m_panes && m_panes->count() == 1) {
        if (auto *inner = qobject_cast<QSplitter *>(m_panes->widget(0))) {
            m_panes->setOrientation(inner->orientation());
            splice(m_panes, inner);
        }
    }
}

void MainWindow::startTabDrag(QTabWidget *pane)
{
    QWidget *v = pane->currentWidget();
    if (!v)
        return;
    QTabBar *bar = pane->tabBar();
    // Ends the tab bar's own reordering before the drag takes over the mouse.
    QMouseEvent release(QEvent::MouseButtonRelease, m_tabPress, bar->mapToGlobal(m_tabPress), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(tabMime), QByteArray::number(quintptr(v)));
    auto *drag = new QDrag(bar);
    drag->setMimeData(mime);
    drag->setPixmap(bar->grab(bar->tabRect(pane->indexOf(v))));
    for (QTabWidget *p : panes()) {
        QWidget *zone = p->findChild<QWidget *>(u"dropZone"_s, Qt::FindDirectChildrenOnly);
        zone->setGeometry(p->rect());
        zone->raise();
        zone->show();
    }
    drag->exec(Qt::MoveAction);
    for (QTabWidget *p : panes())
        p->findChild<QWidget *>(u"dropZone"_s, Qt::FindDirectChildrenOnly)->hide();
}

void MainWindow::moveTab(QWidget *v, QTabWidget *target, int zone)
{
    QTabWidget *from = paneOf(v);
    if (!from || (zone == DropZone::Center && from == target) || (from == target && from->count() == 1))
        return;
    if (zone != DropZone::Center) {
        const bool across = zone == DropZone::Left || zone == DropZone::Right;
        target = insertPane(target, across ? Qt::Horizontal : Qt::Vertical, zone == DropZone::Right || zone == DropZone::Bottom);
    }
    const int i = from->indexOf(v);
    const QString text = from->tabText(i), tip = from->tabToolTip(i);
    from->removeTab(i);
    const int j = target->addTab(v, text);
    target->setTabToolTip(j, tip);
    updateTabButton(v);
    target->setCurrentIndex(j);
    dropEmptyPane(from);
    m_tabs = target;
    onCurrentChanged();
    if (auto *note = qobject_cast<NoteView *>(v))
        note->focusContent();
    else
        v->setFocus();
}

NoteView *MainWindow::currentView() const
{
    return qobject_cast<NoteView *>(m_tabs->currentWidget());
}

QList<NoteView *> MainWindow::views() const
{
    QList<NoteView *> out;
    for (QTabWidget *pane : panes())
        for (int i = 0; i < pane->count(); ++i)
            if (auto *v = qobject_cast<NoteView *>(pane->widget(i)))
                out << v;
    return out;
}

NoteView *MainWindow::createTab()
{
    auto *v = new NoteView(m_vault);
    v->applyFont(noteFont());
    v->setLivePreview(m_livePreview);
    v->setLinkCandidates(m_candidates);
    applySettings(v);
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
    connect(v, &NoteView::hoverLink, this, &MainWindow::previewLink);
    connect(v, &NoteView::notice, this, [this](const QString &message) { statusBar()->showMessage(message, 15000); });
    // The same note open in another pane takes up what this one saved.
    connect(v, &NoteView::saved, this, [this, v] {
        for (NoteView *other : views())
            if (other != v && other->path() == v->path())
                other->reloadFromDisk();
    });
    v->editor()->setDocumentLookup([this](const QString &path) -> QTextDocument * {
        for (NoteView *other : views())
            if (other->path() == path && other->isNote())
                return other->editor()->document();
        return nullptr;
    });
    connect(v, &NoteView::pathChanged, this, [this, v] {
        QTabWidget *pane = paneOf(v);
        if (!pane)
            return;
        const int i = pane->indexOf(v);
        pane->setTabText(i, Vault::isNote(v->path()) ? Vault::baseName(v->path()) : v->path().section(u'/', -1));
        pane->setTabToolTip(i, v->path());
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
    m_tabs->addTab(v, QString());
    updateTabButton(v);
    return v;
}

// A pinned tab shows a pin instead of the close button; clicking it unpins.
void MainWindow::updateTabButton(QWidget *page)
{
    QTabWidget *pane = paneOf(page);
    if (!pane)
        return;
    auto *v = qobject_cast<NoteView *>(page);
    const bool pinned = v && v->isPinned();
    auto *button = new QToolButton;
    button->setIcon(Theme::icon(pinned ? u"pin"_s : u"close"_s));
    button->setIconSize(QSize(14, 14));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(pinned ? tr("Unpin") : tr("Close tab (Ctrl+W)"));
    connect(button, &QToolButton::clicked, this, [this, page, v] {
        if (v && v->isPinned()) {
            v->setPinned(false);
            updateTabButton(v);
        } else {
            closeTab(page);
        }
    });
    pane->tabBar()->setTabButton(pane->indexOf(page), QTabBar::RightSide, button);
}

QList<QWidget *> MainWindow::tabPages() const
{
    QList<QWidget *> out;
    for (QTabWidget *pane : panes())
        for (int i = 0; i < pane->count(); ++i)
            out << pane->widget(i);
    return out;
}

void MainWindow::closeTab(QWidget *page)
{
    if (auto *v = qobject_cast<NoteView *>(page))
        return closeView(v);
    QTabWidget *pane = paneOf(page);
    if (!pane)
        return;
    pane->removeTab(pane->indexOf(page));
    page->deleteLater();
    dropEmptyPane(pane);
    onCurrentChanged();
}

void MainWindow::closeView(NoteView *v, bool saveFirst)
{
    QTabWidget *pane = paneOf(v);
    if (!pane)
        return;
    if (saveFirst && !v->discardIfUntouched()) {
        v->save();
        m_closedTabs.removeAll(v->path());
        m_closedTabs << v->path();
    }
    pane->removeTab(pane->indexOf(v));
    v->deleteLater();
    dropEmptyPane(pane);
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
    if (newTab || !v || (v->isPinned() && v->path() != path))
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
        const QString file = name.endsWith(u".md"_s, Qt::CaseInsensitive) ? name : name + u".md"_s;
        const QString rel = file.contains(u'/') ? file : join(defaultNoteDir(), file);
        if (const QString problem = Vault::nameProblem(Vault::baseName(rel)); !problem.isEmpty()) {
            QMessageBox::warning(this, tr("Could not create note"), problem);
            return;
        }
        path = m_vault->createNote(rel);
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("Could not create note"), tr("Could not create “%1”.").arg(rel));
            return;
        }
        openPath(path, newTab, heading);
        // Left empty, a note made by following a link is removed again, like any new note.
        if (NoteView *v = currentView(); v && v->path() == path)
            v->markNew();
        return;
    }
    openPath(path, newTab, heading);
}

bool MainWindow::validName(const QString &name)
{
    const QString problem = Vault::nameProblem(name);
    if (!problem.isEmpty())
        QMessageBox::warning(this, tr("Invalid name"), problem);
    return problem.isEmpty();
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
    deletePaths({path});
}

void MainWindow::deletePaths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    const QString what = paths.size() == 1 ? paths.first() : tr("%n items", nullptr, int(paths.size()));
    if (confirmDeletion() && QMessageBox::question(this, tr("Delete"), tr("Move “%1” to the trash?").arg(what)) != QMessageBox::Yes)
        return;
    for (const QString &path : paths) {
        for (NoteView *v : views())
            if (v->path() == path || v->path().startsWith(path + u'/'))
                closeView(v, false);
        if (!m_vault->remove(path))
            QMessageBox::warning(this, tr("Delete failed"), tr("Could not move “%1” to the trash.").arg(path));
    }
}

bool MainWindow::confirmDeletion() const
{
    return Settings::confirmDelete(*m_vault);
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
    connect(button(u"file-plus"_s, tr("New note")), &QToolButton::clicked, this, [this] { newNote(defaultNoteDir()); });
    connect(button(u"folder-plus"_s, tr("New folder")), &QToolButton::clicked, this, [this] { newFolder({}); });
    connect(button(u"calendar"_s, tr("Open today's daily note")), &QToolButton::clicked, this, &MainWindow::openDailyNote);

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

void MainWindow::showTabMenu(QTabWidget *pane, const QPoint &pos)
{
    QTabBar *bar = pane->tabBar();
    QWidget *page = pane->widget(bar->tabAt(pos));
    if (!page || !m_vault)
        return;
    QMenu menu(this);
    if (auto *v = qobject_cast<NoteView *>(page))
        fillNoteMenu(&menu, v, true);
    else
        addCloseActions(&menu, pane, page);
    menu.exec(bar->mapToGlobal(pos));
}

void MainWindow::addCloseActions(QMenu *menu, QTabWidget *pane, QWidget *page)
{
    auto others = [pane, page](bool rightOnly) {
        QList<QWidget *> out;
        for (int i = rightOnly ? pane->indexOf(page) + 1 : 0; i < pane->count(); ++i) {
            auto *v = qobject_cast<NoteView *>(pane->widget(i));
            if (pane->widget(i) != page && !(v && v->isPinned()))
                out << pane->widget(i);
        }
        return out;
    };
    menu->addAction(tr("Close"), this, [this, page] { closeTab(page); });
    menu->addAction(tr("Close others"), this, [this, others] {
        for (QWidget *o : others(false))
            closeTab(o);
    })->setEnabled(!others(false).isEmpty());
    menu->addAction(tr("Close tabs to the right"), this, [this, others] {
        for (QWidget *o : others(true))
            closeTab(o);
    })->setEnabled(!others(true).isEmpty());
    menu->addSeparator();
}

// The tab menu and the note's "More options" menu, in Obsidian's order.
void MainWindow::fillNoteMenu(QMenu *menu, NoteView *v, bool tab)
{
    const QString path = v->path();
    if (QTabWidget *pane = paneOf(v); tab && pane) {
        addCloseActions(menu, pane, v);
        menu->addAction(v->isPinned() ? tr("Unpin") : tr("Pin"), this, [this, v] {
            v->setPinned(!v->isPinned());
            updateTabButton(v);
        });
        menu->addAction(tr("Split right"), this, [this, pane, v] {
            pane->setCurrentWidget(v);
            setActivePane(pane);
            split(Qt::Horizontal);
        });
        menu->addAction(tr("Split down"), this, [this, pane, v] {
            pane->setCurrentWidget(v);
            setActivePane(pane);
            split(Qt::Vertical);
        });
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
    menu->addAction(tr("Move file to..."), this, [this, path] { chooseFolderAndMove({path}); });
    menu->addAction(m_bookmarks->has(path) ? tr("Remove bookmark") : tr("Bookmark"), this, [this, path] { m_bookmarks->toggle(path); });
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
    if (v->isNote()) {
        menu->addAction(tr("Version history…"), this, [this, path] { openRecovery(path); });
        menu->addAction(tr("Export to PDF…"), this, [this, v] { exportNote(v, u"pdf"_s); });
    }
    menu->addSeparator();
    menu->addAction(tr("Delete file"), this, [this, path] { deletePath(path); });
}

void MainWindow::addFileActions(QMenu *menu, const QString &path, bool deletable)
{
    PathActions::addCopyMenu(menu, *m_vault, path);
    menu->addSeparator();
    menu->addAction(tr("Rename..."), this, [this, path] { promptRename(path); });
    menu->addAction(tr("Move file to..."), this, [this, path] { chooseFolderAndMove({path}); });
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

void MainWindow::chooseFolderAndMove(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    const QString from = paths.size() == 1 ? dirOf(paths.first()) : QString(u'\x01');
    const QString root = tr("(vault root)");
    QStringList folders;
    if (!from.isEmpty())
        folders << root;
    QDirIterator it(m_vault->root(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = m_vault->relativePath(it.next());
        const bool hidden = dir.startsWith(u'.') || dir.contains(u"/."_s);
        const bool inside = std::any_of(paths.cbegin(), paths.cend(), [&](const QString &p) { return dir == p || dir.startsWith(p + u'/'); });
        if (!hidden && dir != from && !inside)
            folders << dir;
    }
    std::sort(folders.begin() + (from.isEmpty() ? 0 : 1), folders.end());
    const QString what = paths.size() == 1 ? paths.first().section(u'/', -1) : tr("%n items", nullptr, int(paths.size()));
    if (folders.isEmpty()) {
        QMessageBox::information(this, tr("Move to"), tr("There is no other folder to move “%1” to.").arg(what));
        return;
    }
    bool ok = false;
    const QString to = QInputDialog::getItem(this, tr("Move to"), tr("Move “%1” to:").arg(what), folders, 0, false, &ok);
    if (!ok)
        return;
    for (const QString &path : paths)
        if (dirOf(path) != (to == root ? QString() : to))
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

// Saves the note first, so the export has what is on screen.
void MainWindow::exportNote(NoteView *v, const QString &format)
{
    if (!v->isNote())
        return;
    v->save();
    if (format == u"print") {
        QPrinter printer(QPrinter::HighResolution);
        QPrintDialog dialog(&printer, this);
        dialog.setWindowTitle(tr("Print %1").arg(Vault::baseName(v->path())));
        if (dialog.exec() == QDialog::Accepted)
            Export::print(*m_vault, v->path(), Settings::textFont(), &printer);
        return;
    }
    const bool pdf = format == u"pdf";
    QSettings s;
    const QString dir = s.value(u"exportDir"_s, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    QString file = QFileDialog::getSaveFileName(this, pdf ? tr("Export to PDF") : tr("Export to HTML"),
                                                QDir(dir).filePath(Vault::baseName(v->path()) + u'.' + format),
                                                pdf ? tr("PDF files (*.pdf)") : tr("HTML files (*.html *.htm)"));
    if (file.isEmpty())
        return;
    if (QFileInfo(file).suffix().isEmpty())
        file += u'.' + format;
    s.setValue(u"exportDir"_s, QFileInfo(file).absolutePath());
    const bool ok = pdf ? Export::writePdf(*m_vault, v->path(), Settings::textFont(), file)
                    : Export::writeHtml(*m_vault, v->path(), Settings::textFont(), file);
    if (ok)
        statusBar()->showMessage(tr("Exported to %1").arg(QDir::toNativeSeparators(file)), 8000);
    else
        QMessageBox::warning(this, tr("Export failed"), tr("Could not write %1.").arg(QDir::toNativeSeparators(file)));
}

QFont MainWindow::noteFont() const
{
    return Settings::textFont(m_fontDelta);
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
    m_center->setCurrentWidget(tabPages().isEmpty() ? m_empty : static_cast<QWidget *>(m_panes));
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
    for (NoteView *v : views()) {
        if (m_vault->exists(v->path()) || v->path().isEmpty())
            continue;
        // Deleted outside Serat; unsaved words are written back rather than lost with the tab.
        if (v->hasUnsavedChanges() && v->save()) {
            statusBar()->showMessage(tr("“%1” was deleted outside Serat; it was saved again with your unsaved changes.")
                                         .arg(Vault::baseName(v->path())), 15000);
            continue;
        }
        closeView(v, false);
    }
    m_candidates.clear();
    for (const QString &f : m_vault->files())
        m_candidates << m_vault->linkText(f);
    for (NoteView *v : views()) {
        v->setLinkCandidates(m_candidates);
        v->refreshRender();
    }
    m_search->rerun();
    m_tags->showFor(m_vault);
    for (QWidget *page : tabPages())
        if (auto *graph = page->findChild<GraphView *>())
            graph->refresh();
    refreshPanels();
}

// The whole vault's graph in a tab of its own; a second Ctrl+G goes back to it.
void MainWindow::openGraph()
{
    for (QWidget *page : tabPages()) {
        if (qobject_cast<NoteView *>(page))
            continue;
        QTabWidget *pane = paneOf(page);
        pane->setCurrentWidget(page);
        setActivePane(pane);
        page->findChild<GraphView *>()->setFocus();
        return;
    }
    QWidget *page = createGraphTab();
    m_tabs->setCurrentWidget(page);
    onCurrentChanged();
    page->findChild<GraphView *>()->setFocus();
}

QWidget *MainWindow::createGraphTab()
{
    auto *page = new QWidget;
    page->setObjectName(u"graphTab"_s);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Show notes whose path contains…"));
    filter->setClearButtonEnabled(true);
    auto *graph = new GraphView;
    graph->setFocusPolicy(Qt::ClickFocus);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(10, 8, 10, 8);
    bar->addWidget(filter);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(graph, 1);
    connect(filter, &QLineEdit::textChanged, graph, &GraphView::setFilter);
    // Notes open beside the graph, which stays in its tab.
    connect(graph, &GraphView::openRequested, this, [this, page](const QString &p, bool) {
        if (QTabWidget *pane = paneOf(page))
            setActivePane(pane);
        openPath(p, true);
    });
    graph->setScope(m_vault, {});
    const int i = m_tabs->addTab(page, tr("Graph view"));
    m_tabs->setTabToolTip(i, tr("Graph view"));
    updateTabButton(page);
    return page;
}

void MainWindow::refreshPanels()
{
    NoteView *v = currentView();
    const QString path = v ? v->path() : QString();
    m_backlinks->showFor(m_vault, path);
    m_outgoing->showFor(m_vault, path);
    m_outline->showFor(m_vault ? m_vault->note(path) : nullptr);
    m_localGraph->setScope(m_vault, path, 1);
    const int sources = m_backlinks->sourceCount();
    m_backlinkCount->setText(sources == 1 ? tr("1 backlink") : tr("%1 backlinks").arg(sources));
    m_backlinkCount->setVisible(m_vault && !path.isEmpty());
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
    else if (!v && m_tabs->currentWidget())
        parts << m_tabs->tabText(m_tabs->currentIndex());
    if (m_vault)
        parts << m_vault->name();
    parts << u"Serat"_s;
    setWindowTitle(parts.join(u" - "_s));
}

void MainWindow::previewLink(const QString &href, const QPoint &globalPos)
{
    m_previewHref = href;
    m_previewPos = globalPos;
    if (href.isEmpty()) {
        m_previewTimer.stop();
        if (!m_preview->underMouse())
            m_preview->hide();
        return;
    }
    m_previewTimer.start();
}

void MainWindow::showPreview()
{
    if (!m_vault || !m_previewHref.startsWith(u"wiki:"_s))
        return;
    const QString target = QUrl::fromPercentEncoding(m_previewHref.mid(5).toUtf8());
    const QString name = target.section(u'#', 0, 0);
    const QString heading = target.section(u'#', 1);
    NoteView *v = currentView();
    const QString path = name.isEmpty() && v ? v->path() : m_vault->resolve(name, v ? v->path() : QString());
    const Note *note = m_vault->note(path);
    if (!note)
        return;
    RenderOptions opt;
    opt.vault = m_vault;
    opt.notePath = path;
    opt.contentWidth = 380;
    opt.palette = palette();
    opt.dark = palette().color(QPalette::Base).lightness() < 128;
    opt.depth = 1;
    const QString title = u"<p style=\"font-size:large;\"><b>%1</b></p>"_s.arg(Vault::baseName(path).toHtmlEscaped());
    Reader *reader = m_preview->reader();
    reader->document()->setDefaultFont(noteFont());
    reader->document()->setDefaultStyleSheet(markdownStyleSheet(palette(), noteFont().pointSizeF()));
    m_preview->popup(title + renderMarkdown(note->text, opt).html, heading.startsWith(u'^') ? QString() : headingAnchor(heading),
                     m_previewPos);
}

QString MainWindow::defaultNoteDir() const
{
    NoteView *v = currentView();
    return m_vault ? Settings::newNoteDir(*m_vault, v ? v->path() : QString()) : QString();
}

void MainWindow::openDailyNote()
{
    if (!m_vault)
        return;
    const QDateTime now = QDateTime::currentDateTime();
    const Settings::Daily daily = Settings::daily(*m_vault);
    QStringList parts = Settings::formatMoment(now, daily.format.isEmpty() ? u"YYYY-MM-DD"_s : daily.format).split(u'/', Qt::SkipEmptyParts);
    for (QString &part : parts)
        part = Vault::safeName(part); // a format may produce ':' and the like
    const QString path = join(daily.folder, parts.join(u'/') + u".md"_s);
    if (!m_vault->exists(path)) {
        QString text;
        if (!daily.templatePath.isEmpty()) {
            const QString templ = m_vault->resolve(daily.templatePath, {});
            if (m_vault->exists(templ))
                text = Settings::applyTemplate(m_vault->readText(templ), Vault::baseName(path), Settings::templates(*m_vault), now);
            else
                statusBar()->showMessage(tr("Daily note template “%1” was not found.").arg(daily.templatePath), 5000);
        }
        if (m_vault->createNote(path, text).isEmpty()) {
            QMessageBox::warning(this, tr("Daily note"), tr("Could not create “%1”.").arg(path));
            return;
        }
    }
    openPath(path, false);
    if (NoteView *v = currentView())
        v->focusContent();
}

void MainWindow::insertTemplate(Editor *editor)
{
    const Settings::Templates formats = Settings::templates(*m_vault);
    if (formats.folder.isEmpty()) {
        if (QMessageBox::question(this, tr("Insert template"), tr("Choose a template folder in Settings > Templates first. Open Settings now?"))
            == QMessageBox::Yes)
            openSettings();
        return;
    }
    QStringList names;
    QHash<QString, QString> paths;
    for (const QString &f : m_vault->files()) {
        if (!Vault::isNote(f) || !f.startsWith(formats.folder + u'/'))
            continue;
        const QString name = f.mid(formats.folder.size() + 1).chopped(3);
        names << name;
        paths.insert(name, f);
    }
    if (names.isEmpty()) {
        QMessageBox::information(this, tr("Insert template"), tr("There are no notes in the template folder “%1”.").arg(formats.folder));
        return;
    }
    names.sort(Qt::CaseInsensitive);
    const QString chosen = pickItem(this, tr("Insert template"), tr("Find a template…"), names);
    if (chosen.isEmpty())
        return;
    NoteView *v = currentView();
    const QString title = v ? Vault::baseName(v->path()) : QString();
    editor->insertPlainText(Settings::applyTemplate(m_vault->readText(paths.value(chosen)), title, formats, QDateTime::currentDateTime()));
    editor->setFocus();
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(m_vault, menuBar(), this);
    connect(&dlg, &SettingsDialog::changed, this, qOverload<>(&MainWindow::applySettings));
    dlg.exec();
    applySettings();
}

void MainWindow::applySettings()
{
    const bool live = Settings::app(u"livePreview"_s, true).toBool();
    if (live != m_livePreview)
        m_commands.value(u"view.live"_s)->trigger();
    setReadableLineLength(Settings::app(u"readableWidth"_s, true).toBool());
    zoom(0);
    Spelling::instance().reload();
    for (NoteView *v : views())
        applySettings(v);
}

void MainWindow::applySettings(NoteView *v)
{
    v->editor()->setAutoPair(Settings::app(u"autoPairBrackets"_s, true).toBool(), Settings::app(u"autoPairMarkdown"_s, true).toBool());
    v->editor()->setFoldingShown(Settings::app(u"foldGutter"_s, true).toBool(), Settings::app(u"indentGuides"_s, true).toBool());
    v->relayout();
}

void MainWindow::openRecovery(const QString &path)
{
    if (!m_vault)
        return;
    RecoveryDialog dlg(m_vault, path, this);
    if (dlg.exec() != QDialog::Accepted || dlg.path().isEmpty())
        return;
    const QString target = dlg.path();
    if (!m_vault->exists(target)) {
        if (m_vault->createNote(target, dlg.text()).isEmpty()) {
            QMessageBox::warning(this, tr("File recovery"), tr("Could not recreate “%1”.").arg(target));
            return;
        }
        openPath(target, false);
        statusBar()->showMessage(tr("“%1” was restored.").arg(Vault::baseName(target)), 8000);
        return;
    }
    openPath(target, false);
    NoteView *v = currentView();
    if (!v || v->path() != target)
        return;
    // Through the editor, so Ctrl+Z takes the restore back; the text it replaces becomes a version.
    m_vault->recovery().snapshot(target, v->editor()->toPlainText(), true);
    QTextCursor c(v->editor()->document());
    c.select(QTextCursor::Document);
    c.insertText(dlg.text());
    v->save();
    statusBar()->showMessage(tr("“%1” was restored to an earlier version.").arg(Vault::baseName(target)), 8000);
}

// Turns an unlinked mention into a link, keeping the words as written.
void MainWindow::linkMention(const Mention &mention, const QString &target)
{
    const QString words = mention.text.mid(mention.start, mention.length);
    const QString name = m_vault->linkText(target);
    const QString link = u"[["_s + name + (words == name ? QString() : u'|' + words) + u"]]"_s;
    for (NoteView *v : views()) {
        if (v->path() != mention.path || !v->isNote())
            continue;
        const QTextBlock b = v->editor()->document()->findBlockByNumber(mention.line);
        if (b.text() == mention.text) {
            QTextCursor c(b);
            c.setPosition(b.position() + mention.start);
            c.setPosition(b.position() + mention.start + mention.length, QTextCursor::KeepAnchor);
            c.insertText(link);
            v->save();
        }
        refreshPanels();
        return;
    }
    QString text = m_vault->readText(mention.path);
    const bool crlf = text.contains(u"\r\n"_s);
    QStringList lines = text.replace(u"\r\n"_s, u"\n"_s).split(u'\n');
    if (lines.value(mention.line) == mention.text) {
        lines[mention.line].replace(mention.start, mention.length, link);
        text = lines.join(u'\n');
        m_vault->writeNote(mention.path, crlf ? text.replace(u'\n', u"\r\n"_s) : text);
    }
    refreshPanels();
}
