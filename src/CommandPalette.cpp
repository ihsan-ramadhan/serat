#include "CommandPalette.h"

#include "QuickSwitcher.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

constexpr int ShortcutRole = Qt::UserRole + 1;

QString cleanText(const QAction *a)
{
    return a->text().remove(u'&').remove(u'…').trimmed();
}

void collect(QMenu *menu, const QString &prefix, QList<std::pair<QString, QAction *>> &out)
{
    for (QAction *a : menu->actions()) {
        if (a->isSeparator() || !a->isVisible())
            continue;
        if (QMenu *sub = a->menu())
            collect(sub, cleanText(a) + u": "_s, out);
        else
            out.append({prefix + cleanText(a), a});
    }
}

// Shortcut drawn muted on the right of the row.
class Delegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(p, option, index);
        const QString keys = index.data(ShortcutRole).toString();
        if (keys.isEmpty())
            return;
        p->save();
        p->setPen(option.palette.color(QPalette::PlaceholderText));
        p->drawText(option.rect.adjusted(0, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, keys);
        p->restore();
    }
};

} // namespace

CommandPalette::CommandPalette(QMenuBar *menus, const QStringList &recent, QWidget *parent)
    : QDialog(parent)
    , m_recent(recent)
    , m_edit(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setWindowTitle(tr("Command palette"));
    resize(620, 440);
    for (QAction *a : menus->actions())
        if (QMenu *menu = a->menu())
            collect(menu, QString(), m_commands);
    m_edit->setPlaceholderText(tr("Type a command…"));
    m_edit->installEventFilter(this);
    m_list->setObjectName(u"switcher"_s);
    m_list->setUniformItemSizes(true);
    m_list->setItemDelegate(new Delegate(m_list));
    auto *hint = new QLabel(tr("↑↓ navigate   ↵ run   Esc dismiss"), this);
    hint->setForegroundRole(QPalette::PlaceholderText);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_edit);
    layout->addWidget(m_list, 1);
    layout->addWidget(hint);
    connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(m_list, &QListWidget::itemActivated, this, &CommandPalette::choose);
    refilter();
}

void CommandPalette::refilter()
{
    m_list->clear();
    const QString q = m_edit->text().trimmed();
    QList<std::pair<int, int>> ranked; // score, index
    for (int i = 0; i < m_commands.size(); ++i) {
        const auto &[name, action] = m_commands[i];
        if (!action->isEnabled())
            continue;
        int score = q.isEmpty() ? 0 : fuzzyScore(q, name);
        if (score < 0)
            continue;
        if (const qsizetype r = m_recent.indexOf(name); r >= 0)
            score += 1000 - int(r);
        ranked.append({score, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &[score, i] : std::as_const(ranked)) {
        const auto &[name, action] = m_commands[i];
        auto *item = new QListWidgetItem(name, m_list);
        item->setData(Qt::UserRole, i);
        item->setData(ShortcutRole, action->shortcut().toString(QKeySequence::NativeText));
    }
    m_list->setCurrentRow(0);
}

void CommandPalette::choose()
{
    if (const QListWidgetItem *item = m_list->currentItem()) {
        const auto &[name, action] = m_commands[item->data(Qt::UserRole).toInt()];
        m_chosen = action;
        m_chosenName = name;
        accept();
    }
}

bool CommandPalette::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        switch (static_cast<QKeyEvent *>(e)->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QApplication::sendEvent(m_list, e);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(obj, e);
}
