#include "QuickSwitcher.h"

#include "Vault.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
constexpr int PathRole = Qt::UserRole;
constexpr int CreateRole = Qt::UserRole + 1;
}

int fuzzyScore(const QString &query, const QString &text)
{
    if (query.isEmpty())
        return 0;
    const QString q = query.toLower();
    const QString t = text.toLower();
    int score = 0;
    qsizetype from = 0;
    qsizetype last = -2;
    for (const QChar c : q) {
        if (c.isSpace())
            continue;
        const qsizetype pos = t.indexOf(c, from);
        if (pos < 0)
            return -1;
        score += 1;
        if (pos == last + 1)
            score += 5;
        if (pos == 0 || !t[pos - 1].isLetterOrNumber())
            score += 3;
        last = pos;
        from = pos + 1;
    }
    return score * 10 - int(t.size());
}

QuickSwitcher::QuickSwitcher(const Vault *vault, const QStringList &recent, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_recent(recent)
    , m_edit(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setWindowTitle(tr("Quick switcher"));
    resize(620, 440);
    m_edit->setPlaceholderText(tr("Find or create a note…"));
    m_edit->installEventFilter(this);
    m_list->setUniformItemSizes(true);
    m_list->setObjectName(u"switcher"_s);
    auto *hint = new QLabel(tr("↑↓ navigate   ↵ open   Ctrl+↵ open in new tab   Esc dismiss"), this);
    hint->setForegroundRole(QPalette::PlaceholderText);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_edit);
    layout->addWidget(m_list, 1);
    layout->addWidget(hint);

    connect(m_edit, &QLineEdit::textChanged, this, &QuickSwitcher::refilter);
    connect(m_list, &QListWidget::itemActivated, this, &QuickSwitcher::choose);
    refilter();
}

void QuickSwitcher::refilter()
{
    m_list->clear();
    const QString q = m_edit->text().trimmed();
    QList<QPair<int, QString>> ranked;
    const QStringList &files = m_vault->files();

    if (q.isEmpty()) {
        int rank = 1 << 20;
        for (const QString &r : std::as_const(m_recent))
            if (files.contains(r))
                ranked.append({rank--, r});
        for (const QString &f : files)
            if (Vault::isNote(f) && !m_recent.contains(f))
                ranked.append({0, f});
    } else {
        for (const QString &f : files) {
            const int byName = fuzzyScore(q, Vault::baseName(f));
            const int byPath = fuzzyScore(q, f);
            const int s = qMax(byName >= 0 ? byName + 50 : -1, byPath);
            if (s >= 0)
                ranked.append({s, f});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    }

    bool exact = false;
    for (qsizetype i = 0; i < ranked.size() && i < 60; ++i) {
        const QString &path = ranked[i].second;
        const QString name = Vault::isNote(path) ? Vault::baseName(path) : path.section(u'/', -1);
        const QString dir = path.contains(u'/') ? path.section(u'/', 0, -2) : QString();
        exact |= name.compare(q, Qt::CaseInsensitive) == 0;
        auto *item = new QListWidgetItem(dir.isEmpty() ? name : name + u"    "_s + dir, m_list);
        item->setData(PathRole, path);
        item->setToolTip(path);
    }
    if (!q.isEmpty() && !exact) {
        auto *item = new QListWidgetItem(tr("Create “%1”").arg(q), m_list);
        item->setData(CreateRole, q);
        QFont f = item->font();
        f.setItalic(true);
        item->setFont(f);
    }
    m_list->setCurrentRow(0);
}

void QuickSwitcher::choose()
{
    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    m_path = item->data(PathRole).toString();
    m_create = item->data(CreateRole).toString();
    m_newTab = QApplication::keyboardModifiers() & Qt::ControlModifier;
    accept();
}

bool QuickSwitcher::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        switch (k->key()) {
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
