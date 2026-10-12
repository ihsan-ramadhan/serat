#pragma once

#include <QPlainTextEdit>

class MarkdownHighlighter;
class QCompleter;
class QStringListModel;
class Vault;

// Side margin that keeps text at a readable line length, like Obsidian.
int readableMargin(int available, int maxWidth = 760, int minMargin = 24);

class Editor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit Editor(QWidget *parent = nullptr);

    void setVault(Vault *vault) { m_vault = vault; }
    void setNotePath(const QString &path) { m_notePath = path; }
    void setLinkCandidates(const QStringList &names);
    void applyFont(const QFont &font);
    void goToLine(int line);

Q_SIGNALS:
    void linkActivated(const QString &href, bool newTab);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    void updateCompletion();
    void insertCompletion(const QString &name);
    bool continueList();
    void indentLines(bool outdent);
    QString linkAt(const QTextCursor &cursor) const;
    void updateMargins();
    void retheme();
    QString attach(const QString &absolutePath);

    Vault *m_vault = nullptr;
    QString m_notePath;
    MarkdownHighlighter *m_highlighter;
    QCompleter *m_completer;
    QStringListModel *m_candidates;
    QColor m_codeBg;
};
