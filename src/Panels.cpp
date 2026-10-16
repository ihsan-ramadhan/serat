#include "Panels.h"

#include "Markdown.h"
#include "PathActions.h"
#include "Theme.h"
#include "Vault.h"

#include <QActionGroup>
#include <QApplication>
#include <QCollator>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

using namespace Qt::StringLiterals;

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int LineRole = Qt::UserRole + 1;
constexpr int TargetRole = Qt::UserRole + 2;

QString dirOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

bool ctrlHeld()
{
    return QApplication::keyboardModifiers() & Qt::ControlModifier;
}

void setupList(QTreeWidget *w)
{
    w->setHeaderHidden(true);
    w->setFrameShape(QFrame::NoFrame);
    w->setIndentation(14);
    w->setWordWrap(true);
    w->setTextElideMode(Qt::ElideRight);
    w->setContextMenuPolicy(Qt::CustomContextMenu);
}

void itemMenu(QTreeWidget *w, const QPoint &pos, const Vault *vault, const QString &path,
              const std::function<void(bool newTab)> &open)
{
    QMenu menu(w);
    PathActions::addOpenActions(&menu, w, open);
    if (vault && vault->exists(path)) {
        menu.addSeparator();
        PathActions::addCopyMenu(&menu, *vault, path);
    }
    menu.exec(w->viewport()->mapToGlobal(pos));
}

QTreeWidgetItem *placeholder(QTreeWidget *w, const QString &text)
{
    auto *item = new QTreeWidgetItem(w, {text});
    item->setFlags(Qt::NoItemFlags);
    return item;
}

class FsProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QString root;
    FileTree::Sort mode = FileTree::Sort::NameAsc;

    QVariant data(const QModelIndex &index, int role) const override
    {
        const QVariant v = QSortFilterProxyModel::data(index, role);
        if (role == Qt::DisplayRole && index.column() == 0) {
            QString name = v.toString();
            if (name.endsWith(u".md"_s, Qt::CaseInsensitive))
                name.chop(3);
            return name;
        }
        if (role == Qt::DecorationRole && index.column() == 0) {
            static const QIcon folder = Theme::icon(u"folder"_s, u"folder-open"_s);
            static const QIcon note = Theme::icon(u"file-text"_s);
            static const QIcon image = Theme::icon(u"file-image"_s);
            static const QIcon other = Theme::icon(u"file"_s);
            const auto *fs = static_cast<QFileSystemModel *>(sourceModel());
            const QModelIndex source = mapToSource(index);
            if (fs->isDir(source))
                return folder;
            const QString name = fs->fileName(source);
            return Vault::isNote(name) ? note : Vault::isImage(name) ? image : other;
        }
        return v;
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        auto *fs = static_cast<QFileSystemModel *>(sourceModel());
        const QModelIndex index = fs->index(row, 0, parent);
        if (root.isEmpty() || !fs->filePath(index).startsWith(root + u'/'))
            return true;
        return !fs->fileName(index).startsWith(u'.');
    }

    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override
    {
        auto *fs = static_cast<QFileSystemModel *>(sourceModel());
        const bool dl = fs->isDir(l), dr = fs->isDir(r);
        if (dl != dr)
            return dl;
        static const QCollator collator = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        const int byName = collator.compare(fs->fileName(l), fs->fileName(r));
        // Folders stay A to Z whatever the order, as in the usual file managers.
        if (dl)
            return byName < 0;
        const QFileInfo a = fs->fileInfo(l), b = fs->fileInfo(r);
        switch (mode) {
        case FileTree::Sort::NameAsc: return byName < 0;
        case FileTree::Sort::NameDesc: return byName > 0;
        case FileTree::Sort::ModifiedNew: return a.lastModified() > b.lastModified();
        case FileTree::Sort::ModifiedOld: return a.lastModified() < b.lastModified();
        case FileTree::Sort::CreatedNew: return a.birthTime() > b.birthTime();
        case FileTree::Sort::CreatedOld: return a.birthTime() < b.birthTime();
        }
        return byName < 0;
    }
};

} // namespace

FileTree::FileTree(QWidget *parent)
    : QTreeView(parent)
    , m_fs(new QFileSystemModel(this))
    , m_proxy(new FsProxy(this))
{
    m_fs->setReadOnly(true);
    m_fs->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_proxy->setSourceModel(m_fs);
    m_proxy->setDynamicSortFilter(true);
    setModel(m_proxy);
    for (int c = 1; c < m_fs->columnCount(); ++c)
        hideColumn(c);

    setHeaderHidden(true);
    setFrameShape(QFrame::NoFrame);
    setIndentation(14);
    setAnimated(true);
    setUniformRowHeights(true);
    setSortingEnabled(true);
    sortByColumn(0, Qt::AscendingOrder);
    setEditTriggers(NoEditTriggers);
    setExpandsOnDoubleClick(false);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QTreeView::customContextMenuRequested, this, &FileTree::showMenu);
    setSelectionMode(ExtendedSelection);
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
            return; // Shift+click picks files for a group action
        if (m_fs->isDir(m_proxy->mapToSource(index)))
            setExpanded(index, !isExpanded(index));
        else
            Q_EMIT openRequested(pathAt(index), ctrlHeld());
    });
    connect(m_fs, &QFileSystemModel::directoryLoaded, this, [this] {
        if (!m_pendingReveal.isEmpty())
            reveal(m_pendingReveal);
        if (!m_pendingExpand.isEmpty())
            setExpandedFolders(std::exchange(m_pendingExpand, {}));
    });
}

void FileTree::setVault(Vault *vault)
{
    m_vault = vault;
    static_cast<FsProxy *>(m_proxy)->root = vault->root();
    m_fs->setRootPath(vault->root());
    setRootIndex(m_proxy->mapFromSource(m_fs->index(vault->root())));
}

QString FileTree::pathAt(const QModelIndex &index) const
{
    if (!index.isValid() || !m_vault)
        return {};
    return m_vault->relativePath(m_fs->filePath(m_proxy->mapToSource(index)));
}

void FileTree::reveal(const QString &path)
{
    if (!m_vault || path.isEmpty())
        return;
    const QModelIndex source = m_fs->index(m_vault->absolutePath(path));
    const QModelIndex index = m_proxy->mapFromSource(source);
    if (!index.isValid()) {
        m_pendingReveal = path;
        return;
    }
    m_pendingReveal.clear();
    for (QModelIndex p = index.parent(); p.isValid() && p != rootIndex(); p = p.parent())
        expand(p);
    setCurrentIndex(index);
    scrollTo(index);
}

// Shift+click adds or removes a file from the selection; Ctrl+click stays "open in new tab".
QItemSelectionModel::SelectionFlags FileTree::selectionCommand(const QModelIndex &index, const QEvent *e) const
{
    if (e && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease)) {
        const auto *m = static_cast<const QMouseEvent *>(e);
        if (m->button() == Qt::RightButton && selectionModel()->isSelected(index))
            return QItemSelectionModel::NoUpdate; // the menu acts on the whole selection
        if (m->modifiers() & Qt::ShiftModifier)
            return e->type() == QEvent::MouseButtonPress ? QItemSelectionModel::Toggle | QItemSelectionModel::Rows
                                                          : QItemSelectionModel::NoUpdate;
        return QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
    }
    return QTreeView::selectionCommand(index, e);
}

QStringList FileTree::selectedPaths() const
{
    QStringList out;
    for (const QModelIndex &index : selectionModel()->selectedRows())
        out << pathAt(index);
    return out;
}

void FileTree::showMenu(const QPoint &pos)
{
    if (!m_vault)
        return;
    const QString path = pathAt(indexAt(pos));
    const bool dir = path.isEmpty() || m_vault->isDir(path);

    QMenu menu(this);
    if (const QStringList many = selectedPaths(); many.size() > 1 && many.contains(path)) {
        menu.addAction(tr("Move %n items to...", nullptr, int(many.size())), this, [this, many] { Q_EMIT moveManyRequested(many); });
        menu.addSeparator();
        menu.addAction(tr("Delete %n items", nullptr, int(many.size())), this, [this, many] { Q_EMIT deleteManyRequested(many); });
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }
    if (dir) {
        menu.addAction(tr("New note"), this, [this, path] { Q_EMIT newNoteRequested(path); });
        menu.addAction(tr("New folder"), this, [this, path] { Q_EMIT newFolderRequested(path); });
    } else {
        menu.addAction(tr("Open in new tab"), this, [this, path] { Q_EMIT openRequested(path, true); });
    }
    if (path.isEmpty()) {
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }
    menu.addSeparator();
    menu.addAction(tr("Make a copy"), this, [this, path] { Q_EMIT duplicateRequested(path); });
    menu.addAction(dir ? tr("Move folder to...") : tr("Move file to..."), this, [this, path] { Q_EMIT moveToRequested(path); });
    if (extraActions)
        extraActions(&menu, path);
    if (dir)
        menu.addAction(tr("Search in folder"), this, [this, path] { Q_EMIT searchInFolderRequested(path); });
    menu.addSeparator();
    PathActions::addCopyMenu(&menu, *m_vault, path);
    menu.addSeparator();
    if (!dir)
        menu.addAction(tr("Open in default app"), this, [this, path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(path)));
        });
    menu.addAction(tr("Show in system explorer"), this, [this, path, dir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(dir ? path : dirOf(path))));
    });
    menu.addSeparator();
    menu.addAction(tr("Rename..."), this, [this, path] { Q_EMIT renameRequested(path); });
    menu.addAction(tr("Delete"), this, [this, path] { Q_EMIT deleteRequested(path); });
    menu.exec(viewport()->mapToGlobal(pos));
}

void FileTree::setSort(Sort sort)
{
    static_cast<FsProxy *>(m_proxy)->mode = sort;
    m_proxy->invalidate();
    m_proxy->sort(0, Qt::AscendingOrder);
}

FileTree::Sort FileTree::sortOrder() const
{
    return static_cast<FsProxy *>(m_proxy)->mode;
}

QStringList FileTree::expandedFolders() const
{
    QStringList out;
    const std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int i = 0; i < model()->rowCount(parent); ++i) {
            const QModelIndex index = model()->index(i, 0, parent);
            if (isExpanded(index))
                out << pathAt(index);
            walk(index);
        }
    };
    walk(rootIndex());
    return out + m_pendingExpand;
}

void FileTree::setExpandedFolders(const QStringList &folders)
{
    if (!m_vault)
        return;
    for (const QString &folder : folders) {
        const QModelIndex index = m_proxy->mapFromSource(m_fs->index(m_vault->absolutePath(folder)));
        if (index.isValid())
            expand(index);
        else if (m_vault->isDir(folder))
            m_pendingExpand << folder;
    }
}

bool FileTree::anyExpanded() const
{
    for (int i = 0; i < model()->rowCount(rootIndex()); ++i)
        if (isExpanded(model()->index(i, 0, rootIndex())))
            return true;
    return false;
}

void FileTree::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const
{
    if (selectionModel()->isSelected(index))
        painter->fillRect(rect, palette().color(QPalette::Highlight));
    if (!m_fs->isDir(m_proxy->mapToSource(index)))
        return;
    static const QIcon closed = Theme::icon(u"chevron-right"_s);
    static const QIcon open = Theme::icon(u"chevron-down"_s);
    const int size = 12;
    const QRect cell(rect.right() - indentation() + 1, rect.top(), indentation(), rect.height());
    const QRect icon(cell.center().x() - size / 2 + 1, cell.center().y() - size / 2 + 1, size, size);
    (isExpanded(index) ? open : closed).paint(painter, icon);
}

void FileTree::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton) {
        const QModelIndex index = indexAt(e->position().toPoint());
        if (index.isValid() && !m_fs->isDir(m_proxy->mapToSource(index)))
            Q_EMIT openRequested(pathAt(index), true);
        return;
    }
    QTreeView::mouseReleaseEvent(e);
}

void FileTree::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void FileTree::dragMoveEvent(QDragMoveEvent *e)
{
    // The model is read-only, so the base class would refuse every drop.
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void FileTree::dropEvent(QDropEvent *e)
{
    if (!m_vault || !e->mimeData()->hasUrls())
        return;
    QString target = pathAt(indexAt(e->position().toPoint()));
    if (!target.isEmpty() && !m_vault->isDir(target))
        target = dirOf(target);

    const QString root = m_vault->root() + u'/';
    for (const QUrl &url : e->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString abs = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (abs.startsWith(root)) {
            const QString from = m_vault->relativePath(abs);
            if (from != target && dirOf(from) != target)
                Q_EMIT moveRequested(from, target);
        } else if (QFileInfo(abs).isFile()) {
            const QString name = QFileInfo(abs).fileName();
            QFile::copy(abs, m_vault->absolutePath(m_vault->uniquePath(target.isEmpty() ? name : target + u'/' + name)));
        }
    }
    // Copy, not move: we did the move ourselves, the view must not remove rows.
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

SearchPanel::SearchPanel(QWidget *parent)
    : QWidget(parent)
    , m_edit(new QLineEdit(this))
    , m_case(new QToolButton(this))
    , m_summary(new QLabel(this))
    , m_results(new QTreeWidget(this))
{
    m_edit->setPlaceholderText(tr("Search vault…"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setToolTip(tr("Words must all match. Also \"exact phrase\", /regex/, -exclude, OR,\n"
                          "file:, path:, tag:, line:( ), section:( ), task:, task-todo:, task-done:"));
    m_summary->setContentsMargins(4, 0, 4, 0);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    setupList(m_results);

    QSettings settings;
    m_case->setIcon(Theme::icon(u"case"_s));
    m_case->setCheckable(true);
    m_case->setChecked(settings.value(u"searchMatchCase"_s, false).toBool());
    m_case->setToolTip(tr("Match case"));
    m_sort = static_cast<Sort>(settings.value(u"searchSort"_s, 0).toInt());
    auto *sort = new QToolButton(this);
    sort->setIcon(Theme::icon(u"sort"_s));
    sort->setToolTip(tr("Change sort order"));
    sort->setPopupMode(QToolButton::InstantPopup);
    auto *sortMenu = new QMenu(sort);
    const std::pair<Sort, QString> orders[] = {
        {Sort::NameAsc, tr("File name (A to Z)")},      {Sort::NameDesc, tr("File name (Z to A)")},
        {Sort::ModifiedNew, tr("Modified time (new to old)")}, {Sort::ModifiedOld, tr("Modified time (old to new)")},
        {Sort::CreatedNew, tr("Created time (new to old)")},   {Sort::CreatedOld, tr("Created time (old to new)")}};
    auto *group = new QActionGroup(sortMenu);
    for (const auto &[order, label] : orders) {
        QAction *a = sortMenu->addAction(label, this, [this, order] {
            m_sort = order;
            QSettings().setValue(u"searchSort"_s, int(order));
            rerun();
        });
        a->setCheckable(true);
        a->setChecked(order == m_sort);
        group->addAction(a);
        if (order == Sort::NameDesc || order == Sort::ModifiedOld)
            sortMenu->addSeparator();
    }
    sort->setMenu(sortMenu);
    auto *bookmark = new QToolButton(this);
    bookmark->setIcon(Theme::icon(u"bookmark"_s));
    bookmark->setToolTip(tr("Bookmark this search"));
    connect(bookmark, &QToolButton::clicked, this, [this] {
        if (!query().isEmpty())
            Q_EMIT bookmarkRequested(query());
    });

    auto *options = new QHBoxLayout;
    options->setContentsMargins(0, 0, 0, 0);
    options->setSpacing(2);
    options->addWidget(m_summary, 1);
    options->addWidget(m_case);
    options->addWidget(sort);
    options->addWidget(bookmark);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->addWidget(m_edit);
    layout->addLayout(options);
    layout->addWidget(m_results, 1);

    m_timer.setSingleShot(true);
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &SearchPanel::rerun);
    connect(m_edit, &QLineEdit::textChanged, &m_timer, qOverload<>(&QTimer::start));
    connect(m_case, &QToolButton::toggled, this, [this](bool on) {
        QSettings().setValue(u"searchMatchCase"_s, on);
        rerun();
    });
    connect(m_results, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        Q_EMIT openRequested(item->data(0, PathRole).toString(), item->data(0, LineRole).toInt(), ctrlHeld());
    });
    connect(m_results, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QTreeWidgetItem *item = m_results->itemAt(pos);
        if (!item)
            return;
        const QString path = item->data(0, PathRole).toString();
        const int line = item->data(0, LineRole).toInt();
        itemMenu(m_results, pos, m_vault, path, [this, path, line](bool newTab) { Q_EMIT openRequested(path, line, newTab); });
    });
}

void SearchPanel::search(const QString &text)
{
    m_edit->setText(text);
    rerun();
}

QString SearchPanel::query() const
{
    return m_edit->text().trimmed();
}

void SearchPanel::focusSearch()
{
    m_edit->setFocus();
    m_edit->selectAll();
}

void SearchPanel::rerun()
{
    m_results->clear();
    m_summary->clear();
    if (!m_vault || query().isEmpty())
        return;

    const QList<SearchHit> hits = m_vault->search(query(), 500, m_case->isChecked());
    QStringList files;
    QHash<QString, QList<const SearchHit *>> byFile;
    for (const SearchHit &hit : hits) {
        if (!byFile.contains(hit.path))
            files << hit.path;
        byFile[hit.path] << &hit;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    QHash<QString, QFileInfo> info;
    if (m_sort != Sort::NameAsc && m_sort != Sort::NameDesc)
        for (const QString &f : std::as_const(files))
            info.insert(f, QFileInfo(m_vault->absolutePath(f)));
    std::sort(files.begin(), files.end(), [&](const QString &a, const QString &b) {
        const QFileInfo fa = info.value(a), fb = info.value(b);
        switch (m_sort) {
        case Sort::NameDesc: return collator.compare(Vault::baseName(a), Vault::baseName(b)) > 0;
        case Sort::ModifiedNew: return fa.lastModified() > fb.lastModified();
        case Sort::ModifiedOld: return fa.lastModified() < fb.lastModified();
        case Sort::CreatedNew: return fa.birthTime() > fb.birthTime();
        case Sort::CreatedOld: return fa.birthTime() < fb.birthTime();
        default: return collator.compare(Vault::baseName(a), Vault::baseName(b)) < 0;
        }
    });
    for (const QString &path : std::as_const(files)) {
        auto *file = new QTreeWidgetItem(m_results, {Vault::baseName(path)});
        file->setData(0, PathRole, path);
        file->setData(0, LineRole, -1);
        file->setToolTip(0, path);
        QFont f = file->font(0);
        f.setBold(true);
        file->setFont(0, f);
        for (const SearchHit *hit : byFile.value(path)) {
            if (hit->line < 0)
                continue;
            auto *line = new QTreeWidgetItem(file, {hit->text.left(240)});
            line->setData(0, PathRole, hit->path);
            line->setData(0, LineRole, hit->line);
        }
    }
    m_results->expandAll();
    m_summary->setText(files.isEmpty() ? tr("No matches") : files.size() == 1 ? tr("1 file") : tr("%1 files").arg(files.size()));
}

TagsPanel::TagsPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    setColumnCount(2);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (const QString tag = item->data(0, PathRole).toString(); !tag.isEmpty())
            Q_EMIT searchRequested(u"tag:#"_s + tag);
    });
}

void TagsPanel::showFor(const Vault *vault)
{
    QSet<QString> open;
    for (QTreeWidgetItemIterator it(this); *it; ++it)
        if ((*it)->isExpanded())
            open.insert((*it)->data(0, PathRole).toString());
    clear();
    QMap<QString, int> counts; // full tag -> notes, including the notes of nested tags
    if (vault) {
        for (const Note &n : vault->notes()) {
            QSet<QString> seen;
            for (const QString &tag : n.tags) {
                const QStringList parts = tag.split(u'/', Qt::SkipEmptyParts);
                for (int i = 1; i <= parts.size(); ++i)
                    seen.insert(parts.mid(0, i).join(u'/'));
            }
            for (const QString &t : std::as_const(seen))
                ++counts[t];
        }
    }
    if (counts.isEmpty()) {
        placeholder(this, tr("No tags found."));
        return;
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    QStringList tags = counts.keys();
    std::sort(tags.begin(), tags.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    QHash<QString, QTreeWidgetItem *> items;
    for (const QString &tag : std::as_const(tags)) {
        const qsizetype slash = tag.lastIndexOf(u'/');
        QTreeWidgetItem *parent = slash < 0 ? nullptr : items.value(tag.left(slash));
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
        item->setText(0, u'#' + tag.mid(slash + 1));
        item->setText(1, QString::number(counts.value(tag)));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(1, palette().color(QPalette::PlaceholderText));
        item->setData(0, PathRole, tag);
        item->setToolTip(0, u'#' + tag);
        item->setExpanded(open.contains(tag));
        items.insert(tag, item);
    }
}

BookmarksPanel::BookmarksPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    setIndentation(0);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const int i = item->data(0, LineRole).toInt();
        if (i < 0 || i >= m_items.size())
            return;
        const Bookmark &b = m_items[i];
        if (b.type == u"search")
            Q_EMIT searchRequested(b.value);
        else
            Q_EMIT openRequested(b.value, ctrlHeld());
    });
    connect(this, &QWidget::customContextMenuRequested, this, &BookmarksPanel::showMenu);
}

void BookmarksPanel::setVault(Vault *vault, const QString &settingsGroup)
{
    if (m_vault)
        disconnect(m_vault, nullptr, this, nullptr);
    m_vault = vault;
    m_group = settingsGroup;
    m_items.clear();
    QSettings settings;
    settings.beginGroup(m_group);
    if (settings.contains(u"bookmarks"_s)) {
        for (const QVariant &v : settings.value(u"bookmarks"_s).toList()) {
            const QVariantMap m = v.toMap();
            m_items.append({m.value(u"type"_s).toString(), m.value(u"value"_s).toString()});
        }
    } else {
        QFile f(vault->root() + u"/.obsidian/bookmarks.json"_s);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonArray items = QJsonDocument::fromJson(f.readAll()).object().value(u"items"_s).toArray();
            for (const QJsonValue &v : items) {
                const QJsonObject o = v.toObject();
                const QString type = o.value(u"type"_s).toString();
                if (type == u"file" || type == u"folder")
                    m_items.append({type, o.value(u"path"_s).toString()});
                else if (type == u"search")
                    m_items.append({type, o.value(u"query"_s).toString()});
            }
        }
    }
    connect(vault, &Vault::renamed, this, &BookmarksPanel::renamed);
    rebuild();
}

bool BookmarksPanel::has(const QString &path) const
{
    return std::any_of(m_items.cbegin(), m_items.cend(), [&](const Bookmark &b) { return b.type != u"search" && b.value == path; });
}

void BookmarksPanel::toggle(const QString &path)
{
    if (has(path))
        m_items.removeIf([&](const Bookmark &b) { return b.type != u"search" && b.value == path; });
    else
        m_items.append({m_vault && m_vault->isDir(path) ? u"folder"_s : u"file"_s, path});
    save();
    rebuild();
}

void BookmarksPanel::addSearch(const QString &query)
{
    if (std::none_of(m_items.cbegin(), m_items.cend(), [&](const Bookmark &b) { return b.type == u"search" && b.value == query; }))
        m_items.append({u"search"_s, query});
    save();
    rebuild();
}

void BookmarksPanel::save() const
{
    QVariantList list;
    for (const Bookmark &b : m_items)
        list << QVariantMap{{u"type"_s, b.type}, {u"value"_s, b.value}};
    QSettings settings;
    settings.beginGroup(m_group);
    settings.setValue(u"bookmarks"_s, list);
}

void BookmarksPanel::rebuild()
{
    clear();
    if (m_items.isEmpty()) {
        placeholder(this, tr("No bookmarks yet. Use Bookmark in a file's menu or the search panel."));
        return;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        const Bookmark &b = m_items[i];
        const QString name = b.type == u"search" ? b.value : b.type == u"folder" ? b.value.section(u'/', -1) : Vault::baseName(b.value);
        auto *item = new QTreeWidgetItem(this, {name});
        item->setIcon(0, Theme::icon(b.type == u"search" ? u"search"_s : b.type == u"folder" ? u"folder"_s : u"file"_s));
        item->setData(0, LineRole, i);
        item->setToolTip(0, b.value);
        if (b.type != u"search" && m_vault && !m_vault->exists(b.value))
            item->setForeground(0, palette().color(QPalette::PlaceholderText));
    }
}

void BookmarksPanel::showMenu(const QPoint &pos)
{
    const QTreeWidgetItem *item = itemAt(pos);
    if (!item || !(item->flags() & Qt::ItemIsEnabled))
        return;
    const int i = item->data(0, LineRole).toInt();
    const Bookmark b = m_items.value(i);
    QMenu menu(this);
    if (b.type == u"file")
        PathActions::addOpenActions(&menu, this, [this, b](bool newTab) { Q_EMIT openRequested(b.value, newTab); });
    if (i > 0)
        menu.addAction(tr("Move up"), this, [this, i] {
            m_items.swapItemsAt(i, i - 1);
            save();
            rebuild();
        });
    if (i + 1 < m_items.size())
        menu.addAction(tr("Move down"), this, [this, i] {
            m_items.swapItemsAt(i, i + 1);
            save();
            rebuild();
        });
    menu.addSeparator();
    menu.addAction(tr("Remove bookmark"), this, [this, i] {
        m_items.removeAt(i);
        save();
        rebuild();
    });
    menu.exec(viewport()->mapToGlobal(pos));
}

void BookmarksPanel::renamed(const QString &from, const QString &to)
{
    bool changed = false;
    for (Bookmark &b : m_items) {
        if (b.type == u"search")
            continue;
        if (b.value == from) {
            b.value = to;
            changed = true;
        } else if (b.value.startsWith(from + u'/')) {
            b.value = to + b.value.mid(from.size());
            changed = true;
        }
    }
    if (changed) {
        save();
        rebuild();
    }
}

BacklinksPanel::BacklinksPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    setColumnCount(2);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, PathRole).toString().isEmpty())
            Q_EMIT openRequested(item->data(0, PathRole).toString(), item->data(0, LineRole).toInt(), ctrlHeld());
    });
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item == m_mentions && !m_showMentions) {
            m_showMentions = true;
            fillMentions();
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item == m_mentions)
            m_showMentions = false;
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QTreeWidgetItem *item = itemAt(pos);
        if (!item || !(item->flags() & Qt::ItemIsEnabled) || item->data(0, PathRole).toString().isEmpty())
            return;
        const QString path = item->data(0, PathRole).toString();
        const int line = item->data(0, LineRole).toInt();
        itemMenu(this, pos, m_vault, path, [this, path, line](bool newTab) { Q_EMIT openRequested(path, line, newTab); });
    });
}

void BacklinksPanel::showFor(const Vault *vault, const QString &path)
{
    clear();
    m_vault = vault;
    m_path = path;
    m_mentions = nullptr;
    m_sources = 0;
    if (!vault || path.isEmpty()) {
        placeholder(this, tr("Open a note to see what links to it."));
        return;
    }
    const auto links = vault->backlinks(path);
    QSet<QString> sources;
    for (const auto &link : links)
        sources.insert(link.first);
    m_sources = int(sources.size());

    auto section = [this](const QString &title) {
        auto *item = new QTreeWidgetItem(this, {title});
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setForeground(0, palette().color(QPalette::PlaceholderText));
        item->setFirstColumnSpanned(true);
        return item;
    };
    QTreeWidgetItem *linked = section(tr("Linked mentions"));
    QTreeWidgetItem *file = nullptr;
    QStringList lines;
    for (const auto &[source, link] : links) {
        if (!file || file->data(0, PathRole).toString() != source) {
            file = new QTreeWidgetItem(linked, {Vault::baseName(source)});
            file->setData(0, PathRole, source);
            file->setData(0, LineRole, -1);
            file->setToolTip(0, source);
            file->setFirstColumnSpanned(true);
            lines = vault->note(source)->text.split(u'\n');
        }
        auto *ctx = new QTreeWidgetItem(file, {lines.value(link.line).trimmed().left(240)});
        ctx->setData(0, PathRole, source);
        ctx->setData(0, LineRole, link.line);
        ctx->setFirstColumnSpanned(true);
    }
    if (links.isEmpty())
        (new QTreeWidgetItem(linked, {tr("No backlinks found.")}))->setFlags(Qt::NoItemFlags);
    linked->setExpanded(true);
    expandItem(linked);
    for (int i = 0; i < linked->childCount(); ++i)
        linked->child(i)->setExpanded(true);

    m_mentions = section(tr("Unlinked mentions"));
    m_mentions->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    m_mentions->setToolTip(0, tr("Notes that name this one without linking to it"));
    if (m_showMentions) {
        m_mentions->setExpanded(true);
        fillMentions();
    }
}

void BacklinksPanel::fillMentions()
{
    if (!m_mentions || !m_vault)
        return;
    qDeleteAll(m_mentions->takeChildren());
    m_found = m_vault->unlinkedMentions(m_path);
    if (m_found.isEmpty()) {
        (new QTreeWidgetItem(m_mentions, {tr("None found.")}))->setFlags(Qt::NoItemFlags);
        m_mentions->setText(0, tr("Unlinked mentions"));
        return;
    }
    QTreeWidgetItem *file = nullptr;
    for (qsizetype i = 0; i < m_found.size(); ++i) {
        const Mention &m = m_found[i];
        if (!file || file->data(0, PathRole).toString() != m.path) {
            file = new QTreeWidgetItem(m_mentions, {Vault::baseName(m.path)});
            file->setData(0, PathRole, m.path);
            file->setData(0, LineRole, -1);
            file->setToolTip(0, m.path);
            file->setFirstColumnSpanned(true);
            file->setExpanded(true);
        }
        auto *item = new QTreeWidgetItem(file, {m.text.trimmed().left(240)});
        item->setData(0, PathRole, m.path);
        item->setData(0, LineRole, m.line);
        auto *link = new QToolButton;
        link->setText(tr("Link"));
        link->setObjectName(u"mentionLink"_s);
        link->setToolTip(tr("Turn “%1” into a link").arg(m.text.mid(m.start, m.length)));
        connect(link, &QToolButton::clicked, this, [this, i] {
            // Re-linking rewrites the note, which refreshes this panel; leave the click handler first.
            const Mention mention = m_found.value(i);
            QTimer::singleShot(0, this, [this, mention] { Q_EMIT linkMentionRequested(mention, m_path); });
        });
        setItemWidget(item, 1, link);
    }
    m_mentions->setText(0, tr("Unlinked mentions (%1)").arg(m_found.size()));
}

OutgoingPanel::OutgoingPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    setRootIsDecorated(false);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        Q_EMIT linkRequested(item->data(0, TargetRole).toString(), ctrlHeld());
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QTreeWidgetItem *item = itemAt(pos);
        if (!item || !(item->flags() & Qt::ItemIsEnabled))
            return;
        const QString target = item->data(0, TargetRole).toString();
        const QString path = m_vault ? m_vault->resolve(target.section(u'#', 0, 0), m_path) : QString();
        itemMenu(this, pos, m_vault, path, [this, target](bool newTab) { Q_EMIT linkRequested(target, newTab); });
    });
}

void OutgoingPanel::showFor(const Vault *vault, const QString &path)
{
    clear();
    m_vault = vault;
    m_path = path;
    const Note *note = vault ? vault->note(path) : nullptr;
    if (!note) {
        placeholder(this, tr("Open a note to see its links."));
        return;
    }
    QStringList seen;
    for (const Link &l : note->links) {
        if (l.target.isEmpty())
            continue;
        const QString target = l.heading.isEmpty() ? l.target : l.target + u'#' + l.heading;
        if (seen.contains(target))
            continue;
        seen << target;
        const QString resolved = vault->resolve(l.target, path);
        QString label = resolved.isEmpty() ? l.target : Vault::baseName(resolved);
        if (!l.heading.isEmpty())
            label += u" › "_s + l.heading;
        auto *item = new QTreeWidgetItem(this, {label});
        item->setData(0, TargetRole, target);
        if (resolved.isEmpty()) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, palette().color(QPalette::PlaceholderText));
            item->setToolTip(0, tr("Not created yet. Click to create."));
        } else {
            item->setToolTip(0, resolved);
        }
    }
    if (seen.isEmpty())
        placeholder(this, tr("No outgoing links."));
}

OutlinePanel::OutlinePanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        Q_EMIT headingActivated(item->data(0, TargetRole).toString());
    });
}

void OutlinePanel::showFor(const Note *note)
{
    clear();
    if (!note) {
        placeholder(this, tr("Open a note to see its headings."));
        return;
    }
    QList<QPair<int, QTreeWidgetItem *>> stack;
    for (const Heading &h : note->headings) {
        while (!stack.isEmpty() && stack.last().first >= h.level)
            stack.removeLast();
        auto *item = stack.isEmpty() ? new QTreeWidgetItem(this) : new QTreeWidgetItem(stack.last().second);
        item->setText(0, h.text);
        item->setToolTip(0, h.text);
        item->setData(0, TargetRole, h.text);
        stack.append({h.level, item});
    }
    if (note->headings.isEmpty())
        placeholder(this, tr("No headings."));
    expandAll();
}
