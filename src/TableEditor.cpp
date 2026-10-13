#include "TableEditor.h"

#include "Markdown.h"
#include "Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QtMath>
#include <QToolButton>

#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace {
constexpr int PadV = 7;
constexpr int TextInset = 8;
constexpr int HtmlRole = Qt::UserRole + 1;
constexpr int MinColumn = 64;
constexpr int RowStrip = 28; // "add row" button under the grid
constexpr int ColStrip = 28; // "add column" button right of the grid
}

namespace MdTable {

QStringList splitRow(const QString &line)
{
    QString s = line.trimmed();
    if (s.startsWith(u'|'))
        s.remove(0, 1);
    if (s.endsWith(u'|') && !s.endsWith(u"\\|"_s))
        s.chop(1);
    QStringList cells;
    QString cur;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == u'\\' && i + 1 < s.size() && s[i + 1] == u'|') {
            cur += u'|';
            ++i;
        } else if (s[i] == u'|') {
            cells << cur.trimmed();
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    cells << cur.trimmed();
    return cells;
}

QString joinRow(const QStringList &cells)
{
    QStringList escaped;
    for (QString c : cells)
        escaped << c.replace(u'|', u"\\|"_s);
    return u"| "_s + escaped.join(u" | "_s) + u" |"_s;
}

bool isDelimiter(const QString &line)
{
    static const QRegularExpression re(u"^\\s*\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?\\s*$"_s);
    return line.contains(u'|') && re.match(line).hasMatch();
}

bool hasPipe(const QString &line)
{
    for (qsizetype i = 0; i < line.size(); ++i)
        if (line[i] == u'|' && (i == 0 || line[i - 1] != u'\\'))
            return true;
    return false;
}

QList<qsizetype> pipePositions(const QString &line)
{
    QList<qsizetype> out;
    for (qsizetype i = 0; i < line.size(); ++i)
        if (line[i] == u'|' && (i == 0 || line[i - 1] != u'\\'))
            out << i;
    return out;
}

bool isAligned(const QStringList &lines)
{
    if (lines.size() < 2)
        return false;
    const QList<qsizetype> first = pipePositions(lines.first());
    return std::all_of(lines.cbegin(), lines.cend(), [&](const QString &l) { return pipePositions(l) == first; });
}

QStringList alignedLines(const QList<QStringList> &models)
{
    const qsizetype cols = models.first().size();
    QList<QStringList> cells = models;
    QList<qsizetype> width(cols, 3);
    for (qsizetype r = 0; r < cells.size(); ++r) {
        cells[r].resize(cols);
        if (r == 1)
            continue;
        for (qsizetype c = 0; c < cols; ++c) {
            cells[r][c].replace(u'|', u"\\|"_s);
            width[c] = qMax(width[c], cells[r][c].size());
        }
    }
    QStringList lines;
    for (qsizetype r = 0; r < cells.size(); ++r) {
        QStringList parts;
        for (qsizetype c = 0; c < cols; ++c) {
            const QString d = models[1].value(c).trimmed();
            const bool left = d.startsWith(u':'), right = d.endsWith(u':');
            if (r == 1) {
                parts << (left ? u":"_s : QString()) + QString(width[c] - left - right, u'-') + (right ? u":"_s : QString());
                continue;
            }
            const QString &t = cells[r][c];
            const qsizetype pad = width[c] - t.size();
            if (left && right)
                parts << QString(pad / 2, u' ') + t + QString(pad - pad / 2, u' ');
            else if (right)
                parts << QString(pad, u' ') + t;
            else
                parts << t + QString(pad, u' ');
        }
        lines << u"| "_s + parts.join(u" | "_s) + u" |"_s;
    }
    return lines;
}

} // namespace MdTable

namespace {

// Shared by painting, measuring and link hit-testing so they agree.
void prepareCell(QTextDocument &doc, const QString &html, const QFont &font, Qt::Alignment align, int width)
{
    doc.setDocumentMargin(0);
    doc.setDefaultFont(font);
    doc.setDefaultStyleSheet(markdownStyleSheet(QApplication::palette(), font.pointSizeF()));
    QTextOption option(align & Qt::AlignHorizontal_Mask);
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(option);
    doc.setHtml(html);
    doc.setTextWidth(width);
}

QRect textRect(const QRect &cell)
{
    return cell.adjusted(TextInset, 0, -TextInset - 1, 0); // the grid line sits inside the cell's right edge
}

class CellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

        const QRect r = textRect(opt.rect);
        QTextDocument doc;
        prepareCell(doc, index.data(HtmlRole).toString(), opt.font, opt.displayAlignment, r.width());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette = opt.palette;
        if (opt.state & QStyle::State_Selected)
            ctx.palette.setColor(QPalette::Text, opt.palette.color(QPalette::HighlightedText));
        p->save();
        p->setClipRect(opt.rect);
        p->translate(r.left(), r.top() + (r.height() - doc.size().height()) / 2);
        doc.documentLayout()->draw(p, ctx);
        p->restore();
    }
};

} // namespace

void TableGrid::keyPressEvent(QKeyEvent *e)
{
    if (state() == EditingState) {
        QTableWidget::keyPressEvent(e);
        return;
    }
    if (e->matches(QKeySequence::Undo)) {
        Q_EMIT undo();
        return;
    }
    if (e->matches(QKeySequence::Redo)) {
        Q_EMIT redo();
        return;
    }
    const QModelIndex cur = currentIndex();
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    switch (e->key()) {
    case Qt::Key_Up:
        if (plain && cur.row() <= 0) {
            Q_EMIT leave(false);
            return;
        }
        break;
    case Qt::Key_Down:
        if (plain && cur.row() == rowCount() - 1) {
            Q_EMIT leave(true);
            return;
        }
        break;
    case Qt::Key_Escape:
        Q_EMIT leave(true);
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        Q_EMIT clearSelected();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (cur.isValid())
            edit(cur);
        return;
    case Qt::Key_Tab:
        if (cur.row() == rowCount() - 1 && cur.column() == columnCount() - 1) {
            Q_EMIT appendRow();
            return;
        }
        break;
    default:
        break;
    }
    QTableWidget::keyPressEvent(e);
    // A key that opens the cell editor comes back ignored and would also reach the note editor.
    e->accept();
}

bool TableGrid::finishEditing()
{
    if (state() != EditingState)
        return false;
    for (QWidget *editor : viewport()->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        if (editor->isVisible()) {
            commitData(editor);
            closeEditor(editor, QAbstractItemDelegate::NoHint);
            return true;
        }
    }
    return false;
}

void TableGrid::wheelEvent(QWheelEvent *e)
{
    // Sideways scrolling stays in a wide table; the rest scrolls the note.
    const QPoint d = e->angleDelta();
    if (horizontalScrollBar()->maximum() > 0 && (qAbs(d.x()) > qAbs(d.y()) || (e->modifiers() & Qt::ShiftModifier))) {
        QTableWidget::wheelEvent(e);
        return;
    }
    // grid -> overlay -> editor viewport
    QWidget *note = parentWidget() ? parentWidget()->parentWidget() : nullptr;
    if (note)
        QCoreApplication::sendEvent(note, e);
}

void TableGrid::mouseReleaseEvent(QMouseEvent *e)
{
    const bool follow = (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
                        || e->button() == Qt::MiddleButton;
    const QModelIndex index = indexAt(e->position().toPoint());
    if (follow && index.isValid()) {
        const QRect r = textRect(visualRect(index));
        QTextDocument doc;
        prepareCell(doc, index.data(HtmlRole).toString(), index.data(Qt::FontRole).value<QFont>(),
                    Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt()), r.width());
        const QPointF origin(r.left(), r.top() + (r.height() - doc.size().height()) / 2);
        const QString href = doc.documentLayout()->anchorAt(e->position() - origin);
        if (!href.isEmpty()) {
            Q_EMIT linkActivated(href, e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ShiftModifier));
            return;
        }
    }
    QTableWidget::mouseReleaseEvent(e);
}

QModelIndex TableGrid::moveCursor(CursorAction action, Qt::KeyboardModifiers mods)
{
    // Tab past the last cell adds a row.
    const QModelIndex cur = currentIndex();
    if (action == MoveNext && cur.row() == rowCount() - 1 && cur.column() == columnCount() - 1) {
        QTimer::singleShot(0, this, &TableGrid::appendRow);
        return {};
    }
    return QTableWidget::moveCursor(action, mods);
}

TableOverlay::TableOverlay(QWidget *parent)
    : QWidget(parent)
    , m_grid(new TableGrid(this))
    , m_addRow(new QToolButton(this))
    , m_addCol(new QToolButton(this))
{
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    setCursor(Qt::ArrowCursor);

    m_grid->setObjectName(u"mdTable"_s);
    m_grid->horizontalHeader()->hide();
    m_grid->verticalHeader()->hide();
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setWordWrap(true);
    m_grid->setTextElideMode(Qt::ElideNone);
    m_grid->setAutoScroll(false);
    m_grid->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked
                            | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setItemDelegate(new CellDelegate(m_grid));

    m_addRow->setObjectName(u"tableAdd"_s);
    m_addRow->setIcon(Theme::icon(u"plus"_s));
    m_addRow->setToolTip(tr("Add row (Tab in the last cell)"));
    m_addRow->setFocusPolicy(Qt::NoFocus);
    m_addCol->setObjectName(u"tableAdd"_s);
    m_addCol->setIcon(Theme::icon(u"plus"_s));
    m_addCol->setToolTip(tr("Add column"));
    m_addCol->setFocusPolicy(Qt::NoFocus);

    auto appendRow = [this] {
        m_grid->finishEditing();
        QList<QStringList> rows = m_rows;
        rows << QStringList(columnCount());
        emitChanged(rows, m_delims, int(rows.size()) - 1, 0, true);
    };
    connect(m_addRow, &QToolButton::clicked, this, appendRow);
    connect(m_grid, &TableGrid::appendRow, this, appendRow);
    connect(m_addCol, &QToolButton::clicked, this, [this] {
        m_grid->finishEditing();
        QList<QStringList> rows = m_rows;
        for (QStringList &r : rows)
            r << QString();
        emitChanged(rows, m_delims + QStringList{u"---"_s}, 0, columnCount(), true);
    });
    connect(m_grid, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating)
            return;
        // Update now: a + click or menu action can come before the write.
        const QString text = item->text().remove(u'\n');
        m_rows[item->row()][item->column()] = text;
        m_updating = true;
        item->setData(HtmlRole, m_render ? m_render(text) : text.toHtmlEscaped());
        m_updating = false;
        m_layoutWidth = -1;
        emitChanged(m_rows, m_delims, item->row(), item->column());
    });
    connect(m_grid, &TableGrid::clearSelected, this, [this] {
        QList<QStringList> rows = m_rows;
        for (const QModelIndex &i : m_grid->selectionModel()->selectedIndexes())
            rows[i.row()][i.column()].clear();
        const QModelIndex cur = m_grid->currentIndex();
        emitChanged(rows, m_delims, cur.row(), cur.column());
    });
    connect(m_grid, &TableGrid::leave, this, &TableOverlay::leave);
    connect(m_grid, &TableGrid::undo, this, &TableOverlay::undo);
    connect(m_grid, &TableGrid::redo, this, &TableOverlay::redo);
    connect(m_grid, &TableGrid::linkActivated, this, &TableOverlay::linkActivated);
    connect(m_grid, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row >= 0)
            Q_EMIT rowFocused(row);
    });
    connect(m_grid, &QWidget::customContextMenuRequested, this, &TableOverlay::showMenu);
}

void TableOverlay::emitChanged(const QList<QStringList> &rows, const QStringList &delims, int row, int col, bool edit)
{
    Q_EMIT changed(rows, delims, row, col, edit);
}

void TableOverlay::setCellRenderer(std::function<QString(const QString &)> render)
{
    m_render = std::move(render);
    restyle();
}

void TableOverlay::setNoteFont(const QFont &font)
{
    m_font = font;
    m_grid->setFont(font);
    restyle();
}

void TableOverlay::setTable(const QList<QStringList> &input, const QStringList &delims)
{
    QList<QStringList> rows = input;
    for (QStringList &r : rows)
        r.resize(delims.size());
    if (rows == m_rows && delims == m_delims)
        return;
    m_rows = rows;
    m_delims = delims;

    m_updating = true;
    m_grid->setRowCount(int(rows.size()));
    m_grid->setColumnCount(int(delims.size()));
    for (int r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < delims.size(); ++c) {
            QTableWidgetItem *item = m_grid->item(r, c);
            if (!item) {
                item = new QTableWidgetItem;
                m_grid->setItem(r, c, item);
            }
            if (item->text() != rows[r][c])
                item->setText(rows[r][c]);
        }
    }
    restyle();
    m_updating = false;
}

Qt::Alignment TableOverlay::alignment(int col) const
{
    const QString d = m_delims.value(col).trimmed();
    if (d.startsWith(u':') && d.endsWith(u':'))
        return Qt::AlignHCenter;
    if (d.endsWith(u':'))
        return Qt::AlignRight;
    return Qt::AlignLeft;
}

void TableOverlay::restyle()
{
    const bool was = m_updating;
    m_updating = true;
    QFont bold = m_font;
    bold.setBold(true);
    for (int r = 0; r < m_grid->rowCount(); ++r) {
        for (int c = 0; c < m_grid->columnCount(); ++c) {
            QTableWidgetItem *item = m_grid->item(r, c);
            if (!item)
                continue;
            item->setTextAlignment(alignment(c) | Qt::AlignVCenter);
            item->setFont(r == 0 ? bold : m_font);
            item->setBackground(r == 0 ? QBrush(Theme::colors().raised) : QBrush());
            const QString text = m_rows.value(r).value(c);
            item->setData(HtmlRole, m_render ? m_render(text) : text.toHtmlEscaped());
        }
    }
    m_updating = was;
    m_layoutWidth = -1;
}

void TableOverlay::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        restyle();
}

QList<int> TableOverlay::layoutFor(int width)
{
    const int cols = columnCount();
    if (cols == 0)
        return {};
    if (width == m_layoutWidth)
        return m_layoutHeights;

    QFont bold = m_font;
    bold.setBold(true);
    QList<QList<QTextDocument *>> docs;
    for (int r = 0; r < m_rows.size(); ++r) {
        docs.append(QList<QTextDocument *>());
        for (int c = 0; c < cols; ++c) {
            auto *doc = new QTextDocument;
            const QTableWidgetItem *item = m_grid->item(r, c);
            prepareCell(*doc, item ? item->data(HtmlRole).toString() : QString(), r == 0 ? bold : m_font, alignment(c), -1);
            docs.last() << doc;
        }
    }

    QList<int> natural(cols, MinColumn);
    for (const auto &row : std::as_const(docs))
        for (int c = 0; c < cols; ++c)
            natural[c] = qMax(natural[c], qCeil(row[c]->idealWidth()) + 2 * TextInset + 2);
    // At least as wide as the longest word (capped), so words wrap whole.
    QList<int> floor(cols, MinColumn);
    const QFontMetrics fmBold(bold), fm(m_font);
    static const QRegularExpression space(u"\\s+"_s);
    for (int r = 0; r < m_rows.size(); ++r)
        for (int c = 0; c < cols; ++c)
            for (const QString &word : m_rows[r][c].split(space, Qt::SkipEmptyParts))
                floor[c] = qMax(floor[c], qMin(200, (r == 0 ? fmBold : fm).horizontalAdvance(word) + 2 * TextInset + 2));

    // Narrow columns keep their width; wide ones share what is left.
    const int avail = qMax(MinColumn, width - ColStrip - 1);
    QList<int> order(cols);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return natural[a] < natural[b]; });
    QList<int> widths(cols);
    int left = avail;
    for (int i = 0; i < cols; ++i) {
        const int c = order[i];
        widths[c] = qMin(natural[c], left / (cols - i));
        left -= widths[c];
    }
    int total = 0;
    for (int c = 0; c < cols; ++c) {
        widths[c] = qMax(widths[c], qMin(floor[c], natural[c]));
        m_grid->setColumnWidth(c, widths[c]);
        total += widths[c];
    }
    m_scrolls = total > avail;
    m_visibleWidth = qMin(total, avail);
    const int scrollBar = m_scrolls ? m_grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_grid) : 0;

    const int minRow = QFontMetrics(m_font).height() + 2 * PadV;
    QList<int> heights;
    const int rows = int(docs.size());
    for (int r = 0; r < rows; ++r) {
        int text = 0;
        for (int c = 0; c < cols; ++c) {
            docs[r][c]->setTextWidth(textRect(QRect(0, 0, widths[c], 1)).width());
            text = qMax(text, qCeil(docs[r][c]->size().height()));
        }
        int h = qMax(minRow, text + 2 * PadV);
        if (r == 0)
            h += 1; // top border
        if (r == rows - 1)
            h += RowStrip + scrollBar;
        heights << h;
    }
    for (const auto &row : std::as_const(docs))
        qDeleteAll(row);
    m_layoutWidth = width;
    m_layoutHeights = heights;
    return heights;
}

void TableOverlay::setLineHeights(const QList<int> &heights)
{
    const int rows = m_grid->rowCount();
    if (heights.size() != rows)
        return;
    const int scrollBar = m_scrolls ? m_grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_grid) : 0;
    int total = 0;
    for (int r = 0; r < rows; ++r) {
        int h = heights[r];
        if (r == 0)
            h -= 1;
        if (r == rows - 1)
            h -= RowStrip + scrollBar;
        m_grid->setRowHeight(r, qMax(8, h));
        total += m_grid->rowHeight(r);
    }
    m_grid->setHorizontalScrollBarPolicy(m_scrolls ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    const int width = m_visibleWidth;
    const int height = total + 1 + scrollBar;
    m_grid->setGeometry(0, 0, width + 1, height);
    m_addCol->setGeometry(width + 5, 0, ColStrip - 6, height);
    m_addRow->setGeometry(0, height + 3, width + 1, RowStrip - 7);
    m_addRow->setIconSize(QSize(16, 16));
    m_addCol->setIconSize(QSize(16, 16));
}

void TableOverlay::focusCell(int row, int col, bool edit)
{
    row = qBound(0, row, m_grid->rowCount() - 1);
    col = qBound(0, col, m_grid->columnCount() - 1);
    m_grid->setFocus();
    m_grid->setCurrentCell(row, col);
    if (edit)
        m_grid->editItem(m_grid->item(row, col));
}

void TableOverlay::showMenu(const QPoint &pos)
{
    m_grid->finishEditing();
    QModelIndex index = m_grid->indexAt(pos);
    if (!index.isValid())
        index = m_grid->currentIndex();
    const int r = qMax(0, index.row());
    const int c = qMax(0, index.column());
    const int cols = columnCount();

    QMenu menu(this);
    auto insertRow = [this, c](int at) {
        QList<QStringList> rows = m_rows;
        rows.insert(at, QStringList(columnCount()));
        emitChanged(rows, m_delims, at, c, true);
    };
    QAction *above = menu.addAction(r > 0 ? tr("Insert row above") : tr("Insert row above (header stays first)"), this,
                                    [=] { insertRow(r); });
    above->setEnabled(r > 0);
    menu.addAction(tr("Insert row below"), this, [=] { insertRow(r + 1); });
    QAction *delRow = menu.addAction(r > 0 ? tr("Delete row") : tr("Delete row (header stays)"), this, [this, r, c] {
        QList<QStringList> rows = m_rows;
        rows.removeAt(r);
        emitChanged(rows, m_delims, qMin(r, int(rows.size()) - 1), c);
    });
    delRow->setEnabled(r > 0);
    menu.addSeparator();

    auto insertCol = [this, r](int at) {
        QList<QStringList> rows = m_rows;
        for (QStringList &row : rows)
            row.insert(at, QString());
        QStringList delims = m_delims;
        delims.insert(at, u"---"_s);
        emitChanged(rows, delims, r, at, true);
    };
    menu.addAction(tr("Insert column left"), this, [=] { insertCol(c); });
    menu.addAction(tr("Insert column right"), this, [=] { insertCol(c + 1); });
    QAction *delCol = menu.addAction(cols > 1 ? tr("Delete column") : tr("Delete column (last one stays)"), this, [this, r, c] {
        QList<QStringList> rows = m_rows;
        for (QStringList &row : rows)
            row.removeAt(c);
        QStringList delims = m_delims;
        delims.removeAt(c);
        emitChanged(rows, delims, r, qMin(c, int(delims.size()) - 1));
    });
    delCol->setEnabled(cols > 1);

    QMenu *align = menu.addMenu(tr("Align column"));
    const Qt::Alignment current = alignment(c);
    const std::pair<QString, Qt::Alignment> options[] = {
        {tr("Left"), Qt::AlignLeft}, {tr("Center"), Qt::AlignHCenter}, {tr("Right"), Qt::AlignRight}};
    for (const auto &[label, a] : options) {
        QAction *act = align->addAction(label, this, [this, r, c, a] {
            QStringList delims = m_delims;
            delims[c] = a == Qt::AlignHCenter ? u":-:"_s : a == Qt::AlignRight ? u"--:"_s : u":--"_s;
            emitChanged(m_rows, delims, r, c);
        });
        act->setCheckable(true);
        act->setChecked(current == a);
    }
    menu.addSeparator();
    menu.addAction(Theme::icon(u"code"_s), tr("Edit as Markdown"), this, &TableOverlay::editSource);
    menu.addAction(tr("Delete table"), this, &TableOverlay::deleteTable);
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
}
