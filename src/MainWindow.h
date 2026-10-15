#pragma once

#include <QMainWindow>
#include <QHash>
#include <QTimer>

class BacklinksPanel;
class FileTree;
class NoteView;
class OutgoingPanel;
class OutlinePanel;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QUrl;
class QVBoxLayout;
class SearchPanel;
class Vault;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openVault(const QString &path);
    void showWelcome();
    // A vault folder, a serat:// link, or nothing (just bring the window up).
    void handleArgument(const QString &arg);
    void openUrl(const QUrl &url);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void buildUi();
    void buildActions();
    void rebuildWelcome();

    NoteView *currentView() const;
    QList<NoteView *> views() const;
    NoteView *createTab();
    void closeTab(int index, bool saveFirst = true);
    void saveAll();

    void openPath(const QString &path, bool newTab, const QString &heading = {}, int line = -1);
    void openLink(const QString &target, const QString &from, bool newTab);
    void newNote(const QString &dir);
    void newFolder(const QString &dir);
    void promptRename(const QString &path);
    void renamePath(const QString &from, const QString &to);
    void movePath(const QString &from, const QString &toDir);
    void deletePath(const QString &path);
    void quickSwitcher(bool newTab = false);
    void commandPalette();
    QWidget *buildFilesPage();
    void fillNoteMenu(QMenu *menu, NoteView *v, bool tab);
    void addFileActions(QMenu *menu, const QString &path);
    void searchVault(const QString &text = {});
    void showTabMenu(const QPoint &pos);
    void duplicatePath(const QString &path);
    void chooseFolderAndMove(const QString &path);
    void revealInTree(const QString &path);
    QString findVault(const QString &name) const;
    void zoom(double delta);

    void onCurrentChanged();
    void onVaultChanged();
    void refreshPanels();
    void updateStatus();
    void updateTitle();

    void saveSession();
    void restoreSession();
    void addRecent(const QString &path);
    QString vaultKey() const;
    QFont noteFont() const;
    bool validName(const QString &name);

    Vault *m_vault = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_welcome = nullptr;
    QVBoxLayout *m_welcomeList = nullptr;
    QSplitter *m_split = nullptr;
    QTabWidget *m_left = nullptr;
    QTabWidget *m_right = nullptr;
    QTabWidget *m_tabs = nullptr;
    FileTree *m_tree = nullptr;
    SearchPanel *m_search = nullptr;
    BacklinksPanel *m_backlinks = nullptr;
    OutgoingPanel *m_outgoing = nullptr;
    OutlinePanel *m_outline = nullptr;
    QLabel *m_vaultLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    QList<QAction *> m_vaultActions;
    QTimer m_refreshTimer;
    QStringList m_recent;
    QStringList m_candidates;
    double m_fontDelta = 0;
    bool m_livePreview = true;
    bool m_autoReveal = false;
    QWidget *m_files = nullptr;
    QHash<QString, QAction *> m_commands;
    QStringList m_closedTabs;
    QStringList m_recentCommands;
};
