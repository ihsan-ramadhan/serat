#include "Panels.h"

#include "Markdown.h"
#include "PathActions.h"
#include "Theme.h"
#include "Vault.h"

#include <QApplication>
#include <QCollator>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QUrl>
#include <QVBoxLayout>

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
        return collator.compare(fs->fileName(l), fs->fileName(r)) < 0;
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
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (m_fs->isDir(m_proxy->mapToSource(index)))
            setExpanded(index, !isExpanded(index));
        else
            Q_EMIT openRequested(pathAt(index), ctrlHeld());
    });
    connect(m_fs, &QFileSystemModel::directoryLoaded, this, [this] {
        if (!m_pendingReveal.isEmpty())
            reveal(m_pendingReveal);
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

void FileTree::showMenu(const QPoint &pos)
{
    if (!m_vault)
        return;
    const QString path = pathAt(indexAt(pos));
    const bool dir = path.isEmpty() || m_vault->isDir(path);
    const QString parentDir = dir ? path : dirOf(path);

    QMenu menu(this);
    if (dir) {
        menu.addAction(tr("New note"), this, [this, parentDir] { Q_EMIT newNoteRequested(parentDir); });
        menu.addAction(tr("New folder"), this, [this, parentDir] { Q_EMIT newFolderRequested(parentDir); });
    } else {
        menu.addAction(tr("Open in new tab"), this, [this, path] { Q_EMIT openRequested(path, true); });
        menu.addAction(tr("Open in default app"), this, [this, path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(path)));
        });
    }
    if (!path.isEmpty()) {
        menu.addSeparator();
        PathActions::addCopyMenu(&menu, *m_vault, path);
        menu.addSeparator();
        if (!dir)
            menu.addAction(tr("Duplicate"), this, [this, path] { Q_EMIT duplicateRequested(path); });
        menu.addAction(tr("Move to…"), this, [this, path] { Q_EMIT moveToRequested(path); });
        menu.addAction(tr("Rename…"), this, [this, path] { Q_EMIT renameRequested(path); });
        menu.addAction(tr("Delete"), this, [this, path] { Q_EMIT deleteRequested(path); });
    }
    menu.addSeparator();
    menu.addAction(tr("Collapse all"), this, &QTreeView::collapseAll);
    menu.addAction(tr("Show in system explorer"), this, [this, parentDir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_vault->absolutePath(parentDir)));
    });
    menu.exec(viewport()->mapToGlobal(pos));
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
    , m_summary(new QLabel(this))
    , m_results(new QTreeWidget(this))
{
    m_edit->setPlaceholderText(tr("Search vault…"));
    m_edit->setClearButtonEnabled(true);
    m_summary->setContentsMargins(4, 0, 4, 0);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    setupList(m_results);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->addWidget(m_edit);
    layout->addWidget(m_summary);
    layout->addWidget(m_results, 1);

    m_timer.setSingleShot(true);
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &SearchPanel::rerun);
    connect(m_edit, &QLineEdit::textChanged, &m_timer, qOverload<>(&QTimer::start));
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

void SearchPanel::focusSearch()
{
    m_edit->setFocus();
    m_edit->selectAll();
}

void SearchPanel::rerun()
{
    m_results->clear();
    m_summary->clear();
    const QString query = m_edit->text().trimmed();
    if (!m_vault || query.isEmpty())
        return;

    const QList<SearchHit> hits = m_vault->search(query);
    QTreeWidgetItem *file = nullptr;
    int files = 0;
    for (const SearchHit &hit : hits) {
        if (!file || file->data(0, PathRole).toString() != hit.path) {
            file = new QTreeWidgetItem(m_results, {Vault::baseName(hit.path)});
            file->setData(0, PathRole, hit.path);
            file->setData(0, LineRole, -1);
            file->setToolTip(0, hit.path);
            QFont f = file->font(0);
            f.setBold(true);
            file->setFont(0, f);
            ++files;
        }
        if (hit.line < 0)
            continue;
        auto *line = new QTreeWidgetItem(file, {hit.text.left(240)});
        line->setData(0, PathRole, hit.path);
        line->setData(0, LineRole, hit.line);
    }
    m_results->expandAll();
    m_summary->setText(files ? tr("%n file(s)", nullptr, files) : tr("No matches"));
}

BacklinksPanel::BacklinksPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setupList(this);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        Q_EMIT openRequested(item->data(0, PathRole).toString(), item->data(0, LineRole).toInt(), ctrlHeld());
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QTreeWidgetItem *item = itemAt(pos);
        if (!item || !(item->flags() & Qt::ItemIsEnabled))
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
    if (!vault || path.isEmpty())
        return;
    const auto links = vault->backlinks(path);
    if (links.isEmpty()) {
        placeholder(this, tr("No backlinks found."));
        return;
    }
    QTreeWidgetItem *file = nullptr;
    QStringList lines;
    for (const auto &[source, link] : links) {
        if (!file || file->data(0, PathRole).toString() != source) {
            file = new QTreeWidgetItem(this, {Vault::baseName(source)});
            file->setData(0, PathRole, source);
            file->setData(0, LineRole, -1);
            file->setToolTip(0, source);
            QFont f = file->font(0);
            f.setBold(true);
            file->setFont(0, f);
            lines = vault->note(source)->text.split(u'\n');
        }
        auto *ctx = new QTreeWidgetItem(file, {lines.value(link.line).trimmed().left(240)});
        ctx->setData(0, PathRole, source);
        ctx->setData(0, LineRole, link.line);
    }
    expandAll();
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
    if (!note)
        return;
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
    if (!note)
        return;
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
