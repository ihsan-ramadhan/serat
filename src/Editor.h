#pragma once

#include <QPlainTextEdit>
#include <QPointer>
#include <QTextBlock>
#include <QTimer>

class MarkdownHighlighter;
class QCompleter;
class QStringListModel;
class TableOverlay;
class Vault;

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
    void setLivePreview(bool on);
    void commitTables();

Q_SIGNALS:
    void linkActivated(const QString &href, bool newTab);
    void hintChanged(const QString &hint);
    void searchRequested(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
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

    struct Deco {
        enum Kind { Bullet, Task, Quote, Rule } kind;
        QRectF rect;
        bool checked = false;
        int pos = 0; // task mark offset in the block
    };
    void toggleWrap(const QString &mark);
    void insertBlock(const QString &text, bool table);
    void insertTask();
    void insertWikilink();
    void updateRawBlocks();
    void updateTableHint();
    bool previewed(const QTextBlock &block) const;
    QList<Deco> decorations(const QTextBlock &block) const;
    QString clickedLink(const QPoint &pos) const;
    QString attach(const QString &absolutePath);

    struct Table {
        QTextBlock head;
        int lines = 0;
        int height = 0;
        TableOverlay *overlay = nullptr;
    };
    void scanTables();
    void layoutTables();
    void positionTables();
    void onCursorMoved();
    int tableIndex(const TableOverlay *overlay) const;
    int tableAt(const QTextBlock &block) const;
    QTextBlock tableLine(const Table &t, int row) const;
    void writeTable(TableOverlay *overlay, const QList<QStringList> &rows, const QStringList &delims, int focusRow,
                    int focusCol, bool edit);
    void leaveTable(TableOverlay *overlay, bool down);
    void editTableSource(TableOverlay *overlay);
    void deleteTable(TableOverlay *overlay);
    void showTableRow(TableOverlay *overlay, int row);
    void setLineHeight(const QTextBlock &block, int height);
    void flushTableWrites();
    void moveCursorQuietly(const QTextCursor &cursor);

    Vault *m_vault = nullptr;
    QString m_notePath;
    MarkdownHighlighter *m_highlighter;
    QCompleter *m_completer;
    QStringListModel *m_candidates;
    QColor m_codeBg;
    QAction *m_bold;
    QAction *m_italic;
    bool m_live = false;
    int m_rawFrom = -1, m_rawTo = -1;
    int m_colFrom = -1, m_colTo = -1;
    QString m_hint;

    QList<Table> m_tables;
    struct PendingWrite {
        QPointer<TableOverlay> overlay;
        QList<QStringList> rows;
        QStringList delims;
        int row, col;
        bool edit;
    };
    QList<PendingWrite> m_pendingWrites;
    QTextBlock m_rawTable;
    int m_rawLines = 0;
    QTimer m_tableTimer;
    bool m_tableBusy = false;
    bool m_navKey = false;
    bool m_inKeyPress = false;
    int m_typedBlock = -1; // block of the last edit made from the keyboard
    bool m_navUp = false;
};
