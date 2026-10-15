#pragma once

#include <QHash>
#include <QImage>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QTextBlock>
#include <QTimer>

#include <functional>

class ImageBlock;
class MarkdownHighlighter;
class PropertiesPanel;
class QMenu;
class QPainter;
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

    // Commands, shared with the menu bar and the command palette.
    enum class Paragraph { Bullet, Numbered, Task, Heading, Body, Quote };
    void toggleWrap(const QString &mark);
    void clearFormatting();
    void setParagraph(Paragraph kind, int level = 0);
    void toggleCheckbox();
    void deleteParagraph();
    void addInternalLink();
    void insertMarkdownLink();
    void insertTable();
    void insertCallout();
    void insertCodeBlock();
    void insertRule();
    void addProperty();
    void pasteAsPlainText();
    void followLinkAtCursor(bool newTab);
    // The context menu shows the window's command actions, plus file actions on vault links.
    void setCommands(std::function<QAction *(const QString &id)> command,
                     std::function<void(QMenu *, const QString &path, bool image)> fileActions);

Q_SIGNALS:
    void linkActivated(const QString &href, bool newTab);
    void hintChanged(const QString &hint);

protected:
    bool event(QEvent *e) override;
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
    // Puts `text` on its own lines; the cursor ends at `cursorAt` within it (-1: the line after it).
    void insertBlock(const QString &text, int cursorAt = -1);
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

    int frontmatterEnd() const;
    void scanProperties(QSet<int> &covered);
    void writeFrontmatter(const QStringList &lines);
    void leaveProperties();
    void showPropertiesSource();
    struct Image {
        QTextBlock block;
        int height = 0;
        ImageBlock *widget = nullptr;
    };
    void scanImages(QSet<int> &covered);
    void showImageMenu(const QString &path, const QPoint &globalPos);
    QPair<QTextBlock, QTextBlock> calloutAt(const QTextBlock &block) const;
    void paintCalloutHead(QPainter &p, const QTextBlock &block);

    Vault *m_vault = nullptr;
    QString m_notePath;
    MarkdownHighlighter *m_highlighter;
    QCompleter *m_completer;
    QStringListModel *m_candidates;
    QStringList m_linkNames;
    bool m_headingMode = false; // completing the heading after '#'
    QColor m_codeBg;
    std::function<QAction *(const QString &)> m_command;
    std::function<void(QMenu *, const QString &, bool)> m_fileActions;
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

    PropertiesPanel *m_props = nullptr;
    int m_propLines = 0; // frontmatter lines under the panel, 0 while it is hidden
    int m_propsHeight = 0;
    bool m_rawProps = false;
    QList<Image> m_images;
    QTextBlock m_rawImage;
    QHash<QString, QImage> m_icons;
};
