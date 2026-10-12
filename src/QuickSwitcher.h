#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;
class Vault;

int fuzzyScore(const QString &query, const QString &text);

class QuickSwitcher : public QDialog
{
    Q_OBJECT
public:
    QuickSwitcher(const Vault *vault, const QStringList &recent, QWidget *parent = nullptr);

    QString selectedPath() const { return m_path; }
    QString createName() const { return m_create; }
    bool newTab() const { return m_newTab; }

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void refilter();
    void choose();

    const Vault *m_vault;
    QStringList m_recent;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QString m_path;
    QString m_create;
    bool m_newTab = false;
};
