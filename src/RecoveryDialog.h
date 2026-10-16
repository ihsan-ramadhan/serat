#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class Vault;

// Earlier versions of notes, including deleted ones; Restore hands one back to the window.
class RecoveryDialog : public QDialog
{
    Q_OBJECT
public:
    RecoveryDialog(Vault *vault, const QString &path, QWidget *parent = nullptr);

    QString path() const { return m_path; }
    QString text() const { return m_text; }

private:
    void fillNotes();
    void fillVersions();

    Vault *m_vault;
    QLineEdit *m_filter;
    QListWidget *m_notes;
    QListWidget *m_versions;
    QPlainTextEdit *m_preview;
    QPushButton *m_restore;
    QPushButton *m_copy;
    QString m_path;
    QString m_text;
};
