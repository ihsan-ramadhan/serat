#include "MainWindow.h"

#include "NoteView.h"
#include "Panels.h"
#include "QuickSwitcher.h"
#include "Theme.h"
#include "Vault.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
    m_left = panelTabs({{m_tree, u"folder", tr("Files")}, {m_search, u"search", tr("Search (Ctrl+Shift+F)")}});

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setObjectName(u"notes"_s);
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->setMovable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->installEventFilter(this);
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
    auto add = [this](QMenu *menu, const QString &text, const QKeySequence &key, auto fn, bool needsVault = true) {
        QAction *a = menu->addAction(text);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        if (needsVault)
            m_vaultActions << a;
        return a;
    };
    auto withView = [this](auto fn) {
        return [this, fn] {
            if (NoteView *v = currentView())
                fn(v);
        };
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    add(file, tr("New note"), QKeySequence::New, [this] { newNote({}); });
    add(file, tr("New folder…"), {}, [this] { newFolder({}); });
    file->addSeparator();
    add(file, tr("Open vault…"), QKeySequence(u"Ctrl+Shift+O"_s), [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Open folder as vault"),
                                                              m_vault ? m_vault->root() : QDir::homePath());
        if (!dir.isEmpty())
            openVault(dir);
    }, false);
    add(file, tr("Close vault"), {}, [this] { showWelcome(); });
    file->addSeparator();
    add(file, tr("Save"), QKeySequence::Save, [this] { saveAll(); });
    add(file, tr("Close tab"), QKeySequence(u"Ctrl+W"_s), [this] {
        if (m_tabs->count())
            closeTab(m_tabs->currentIndex());
    });
    file->addSeparator();
    add(file, tr("Quit"), QKeySequence::Quit, [this] { close(); }, false);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, tr("Find in note"), QKeySequence::Find, withView([](NoteView *v) { v->showFind(); }));
    add(edit, tr("Rename note"), QKeySequence(u"F2"_s), withView([](NoteView *v) { v->focusTitle(); }));

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    add(go, tr("Quick switcher"), QKeySequence(u"Ctrl+O"_s), [this] { quickSwitcher(); });
    add(go, tr("Search vault"), QKeySequence(u"Ctrl+Shift+F"_s), [this] { searchVault(); });
    go->addSeparator();
    QAction *back = add(go, tr("Back"), QKeySequence(u"Alt+Left"_s), withView([](NoteView *v) { v->goBack(); }));
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
    QAction *fwd = add(go, tr("Forward"), QKeySequence(u"Alt+Right"_s), withView([](NoteView *v) { v->goForward(); }));
    fwd->setShortcuts(unique(QList<QKeySequence>{QKeySequence(u"Alt+Right"_s), QKeySequence(u"Ctrl+Alt+Right"_s)}
                             + QKeySequence::keyBindings(QKeySequence::Forward)));
    go->addSeparator();
    add(go, tr("Next tab"), QKeySequence(u"Ctrl+Tab"_s), [this] {
        if (m_tabs->count())
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    })->setShortcuts({QKeySequence(u"Ctrl+Tab"_s), QKeySequence(u"Ctrl+PgDown"_s)});
    add(go, tr("Previous tab"), QKeySequence(u"Ctrl+Shift+Tab"_s), [this] {
        if (m_tabs->count())
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    })->setShortcuts({QKeySequence(u"Ctrl+Shift+Tab"_s), QKeySequence(u"Ctrl+PgUp"_s)});

    QMenu *view = menuBar()->addMenu(tr("&View"));
    add(view, tr("Toggle reading view"), QKeySequence(u"Ctrl+E"_s), withView([](NoteView *v) {
        v->setMode(v->mode() == NoteView::Edit ? NoteView::Read : NoteView::Edit);
    }));
    QAction *live = add(view, tr("Live preview"), QKeySequence(u"Ctrl+Shift+E"_s), [this](bool on) {
        m_livePreview = on;
        QSettings().setValue(u"livePreview"_s, on);
        for (NoteView *v : views())
            v->setLivePreview(on);
    });
    live->setCheckable(true);
    m_livePreview = QSettings().value(u"livePreview"_s, true).toBool();
    live->setChecked(m_livePreview);
    live->setToolTip(tr("Hide Markdown markup except on the line being edited"));
    view->addSeparator();
    add(view, tr("Toggle left sidebar"), QKeySequence(u"Ctrl+\\"_s), [this] { m_left->setVisible(!m_left->isVisible()); });
    add(view, tr("Toggle right sidebar"), QKeySequence(u"Ctrl+Shift+\\"_s), [this] { m_right->setVisible(!m_right->isVisible()); });
    view->addSeparator();
    add(view, tr("Zoom in"), QKeySequence::ZoomIn, [this] { zoom(1); })->setShortcuts({QKeySequence::ZoomIn, QKeySequence(u"Ctrl+="_s)});
    add(view, tr("Zoom out"), QKeySequence::ZoomOut, [this] { zoom(-1); });
    add(view, tr("Reset zoom"), QKeySequence(u"Ctrl+0"_s), [this] { zoom(-m_fontDelta); });
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
    add(help, tr("About Serat"), {}, [this] {
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
    if (saveFirst && !v->discardIfUntouched())
        v->save();
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

void MainWindow::quickSwitcher()
{
    if (!m_vault)
        return;
    QuickSwitcher dlg(m_vault, m_recent, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (!dlg.createName().isEmpty())
        openLink(dlg.createName(), {}, dlg.newTab());
    else
        openPath(dlg.selectedPath(), dlg.newTab());
}

void MainWindow::searchVault()
{
    m_left->show();
    m_left->setCurrentWidget(m_search);
    m_search->focusSearch();
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
    if (NoteView *v = currentView())
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
