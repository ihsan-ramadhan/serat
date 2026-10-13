#pragma once

#include <QTableWidget>
#include <functional>
#include <QTextBlockUserData>

class QToolButton;

namespace MdTable {
// Cells of a pipe-table row, unescaped ("\|" becomes "|") and trimmed.
QStringList splitRow(const QString &line);
QString joinRow(const QStringList &cells);
bool isDelimiter(const QString &line);
bool hasPipe(const QString &line);
// Pipes in the same columns on every line.
bool isAligned(const QStringList &lines);
// models[1] is the delimiter row.
QStringList alignedLines(const QList<QStringList> &models);
}

// The highlighter draws the line transparent at pixelSize so its height matches the grid row.
struct TableLineData : QTextBlockUserData {
    int pixelSize = 0;
};

class TableGrid : public QTableWidget
{
    Q_OBJECT
public:
    using QTableWidget::QTableWidget;
    bool finishEditing();

Q_SIGNALS:
    void appendRow();
    void leave(bool down);
    void clearSelected();
    void undo();
    void redo();
    void linkActivated(const QString &href, bool newTab);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers mods) override;
};

// Grid over a Markdown table in the editor; row 0 is the header.
class TableOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit TableOverlay(QWidget *parent = nullptr);

    void setTable(const QList<QStringList> &rows, const QStringList &delims);
    const QStringList &delims() const { return m_delims; }

    void setNoteFont(const QFont &font);
    void setCellRenderer(std::function<QString(const QString &)> render);
    // Height per source line: header, then body rows (the delimiter is hidden).
    QList<int> layoutFor(int width);
    void setLineHeights(const QList<int> &heights);
    int lineCount() const { return int(m_rows.size()); }
    void focusCell(int row, int col, bool edit);
    void finishEditing() { m_grid->finishEditing(); }

Q_SIGNALS:
    void changed(const QList<QStringList> &rows, const QStringList &delims, int focusRow, int focusCol, bool edit);
    void leave(bool down);
    void editSource();
    void deleteTable();
    void undo();
    void redo();
    void rowFocused(int row);
    void linkActivated(const QString &href, bool newTab);

protected:
    void changeEvent(QEvent *e) override;

private:
    void restyle();
    void showMenu(const QPoint &pos);
    void emitChanged(const QList<QStringList> &rows, const QStringList &delims, int row, int col, bool edit = false);
    Qt::Alignment alignment(int col) const;
    int columnCount() const { return int(m_delims.size()); }

    TableGrid *m_grid;
    QToolButton *m_addRow;
    QToolButton *m_addCol;
    QList<QStringList> m_rows;
    QStringList m_delims;
    QFont m_font;
    std::function<QString(const QString &)> m_render;
    bool m_updating = false;
    // Cached: layoutFor() runs on every keystroke.
    int m_layoutWidth = -1;
    QList<int> m_layoutHeights;
    int m_visibleWidth = 0;
    bool m_scrolls = false;
};
