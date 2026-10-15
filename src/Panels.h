#pragma once

#include <QPointer>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <functional>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QMenu;
class QSortFilterProxyModel;
class QToolButton;
class Vault;
struct Note;

class FileTree : public QTreeView
{
    Q_OBJECT
public:
    explicit FileTree(QWidget *parent = nullptr);
    void setVault(Vault *vault);
    void reveal(const QString &path);

    enum class Sort { NameAsc, NameDesc, ModifiedNew, ModifiedOld, CreatedNew, CreatedOld };
    void setSort(Sort sort);
    Sort sortOrder() const;
    bool anyExpanded() const;
    // Vault-relative folders shown open, including ones inside a collapsed parent.
    QStringList expandedFolders() const;
    void setExpandedFolders(const QStringList &folders);
    QStringList selectedPaths() const;
    // Adds entries the tree cannot decide on itself, such as Bookmark, to a file's menu.
    std::function<void(QMenu *, const QString &path)> extraActions;

Q_SIGNALS:
    void openRequested(const QString &path, bool newTab);
    void newNoteRequested(const QString &dir);
    void newFolderRequested(const QString &dir);
    void renameRequested(const QString &path);
    void deleteRequested(const QString &path);
    void moveRequested(const QString &from, const QString &toDir);
    void moveToRequested(const QString &path);
    void duplicateRequested(const QString &path);
    void searchInFolderRequested(const QString &path);
    void moveManyRequested(const QStringList &paths);
    void deleteManyRequested(const QStringList &paths);

protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &index, const QEvent *e) const override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    QString pathAt(const QModelIndex &index) const;
    void showMenu(const QPoint &pos);

    Vault *m_vault = nullptr;
    QFileSystemModel *m_fs;
    QSortFilterProxyModel *m_proxy;
    QString m_pendingReveal;
    QStringList m_pendingExpand; // waiting for QFileSystemModel to load their parents
};

class SearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanel(QWidget *parent = nullptr);
    void setVault(Vault *vault) { m_vault = vault; rerun(); }
    void focusSearch();
    void search(const QString &text);
    void rerun();
    QString query() const;

    enum class Sort { NameAsc, NameDesc, ModifiedNew, ModifiedOld, CreatedNew, CreatedOld };

Q_SIGNALS:
    void openRequested(const QString &path, int line, bool newTab);
    void bookmarkRequested(const QString &query);

private:
    Vault *m_vault = nullptr;
    QLineEdit *m_edit;
    QToolButton *m_case;
    QLabel *m_summary;
    QTreeWidget *m_results;
    QTimer m_timer;
    Sort m_sort = Sort::NameAsc;
};

// Every tag in the vault, nested at "/", with how many notes use it.
class TagsPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit TagsPanel(QWidget *parent = nullptr);
    void showFor(const Vault *vault);

Q_SIGNALS:
    void searchRequested(const QString &query);
};

class BookmarksPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit BookmarksPanel(QWidget *parent = nullptr);
    // Kept in Serat's settings; seeded once from the vault's own bookmark list, which is never written.
    void setVault(Vault *vault, const QString &settingsGroup);
    bool has(const QString &path) const;
    void toggle(const QString &path);
    void addSearch(const QString &query);

Q_SIGNALS:
    void openRequested(const QString &path, bool newTab);
    void searchRequested(const QString &query);

private:
    struct Bookmark {
        QString type; // file, folder or search
        QString value;
    };
    void rebuild();
    void save() const;
    void showMenu(const QPoint &pos);
    void renamed(const QString &from, const QString &to);

    QPointer<Vault> m_vault;
    QString m_group;
    QList<Bookmark> m_items;
};

class BacklinksPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit BacklinksPanel(QWidget *parent = nullptr);
    void showFor(const Vault *vault, const QString &path);
    int sourceCount() const { return m_sources; }

Q_SIGNALS:
    void openRequested(const QString &path, int line, bool newTab);

private:
    const Vault *m_vault = nullptr;
    int m_sources = 0; // notes linking here
};

class OutgoingPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit OutgoingPanel(QWidget *parent = nullptr);
    void showFor(const Vault *vault, const QString &path);

Q_SIGNALS:
    void linkRequested(const QString &target, bool newTab);

private:
    const Vault *m_vault = nullptr;
    QString m_path;
};

class OutlinePanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit OutlinePanel(QWidget *parent = nullptr);
    void showFor(const Note *note);

Q_SIGNALS:
    void headingActivated(const QString &heading);
};
