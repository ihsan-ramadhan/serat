#pragma once

#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
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

protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;
    void mouseReleaseEvent(QMouseEvent *e) override;
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

Q_SIGNALS:
    void openRequested(const QString &path, int line, bool newTab);

private:
    Vault *m_vault = nullptr;
    QLineEdit *m_edit;
    QLabel *m_summary;
    QTreeWidget *m_results;
    QTimer m_timer;
};

class BacklinksPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit BacklinksPanel(QWidget *parent = nullptr);
    void showFor(const Vault *vault, const QString &path);

Q_SIGNALS:
    void openRequested(const QString &path, int line, bool newTab);

private:
    const Vault *m_vault = nullptr;
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
