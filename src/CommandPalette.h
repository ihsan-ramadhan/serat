#pragma once

#include <QDialog>

class QAction;
class QLineEdit;
class QListWidget;
class QMenuBar;

// Ctrl+P: every menu bar command, searchable, with its shortcut.
class CommandPalette : public QDialog
{
    Q_OBJECT
public:
    CommandPalette(QMenuBar *menus, const QStringList &recent, QWidget *parent = nullptr);
    QAction *chosen() const { return m_chosen; }
    QString chosenName() const { return m_chosenName; }

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void refilter();
    void choose();

    QList<std::pair<QString, QAction *>> m_commands;
    QStringList m_recent;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QAction *m_chosen = nullptr;
    QString m_chosenName;
};
