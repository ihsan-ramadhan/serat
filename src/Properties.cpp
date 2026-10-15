#include "Properties.h"

#include "Theme.h"

#include <QAbstractButton>
#include <QCalendarWidget>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

using namespace Qt::StringLiterals;

using Frontmatter::Property;
using Frontmatter::Type;

namespace {

QString typeIcon(const Property &p)
{
    if (p.key == u"tags")
        return u"tag"_s;
    if (p.key == u"aliases")
        return u"forward"_s;
    switch (p.type) {
    case Type::List: return u"list"_s;
    case Type::Number: return u"hash"_s;
    case Type::Checkbox: return u"square-check"_s;
    case Type::Date: return u"calendar"_s;
    case Type::DateTime: return u"clock"_s;
    case Type::Raw: return u"code"_s;
    default: return u"text"_s;
    }
}

QString typeName(Type t)
{
    switch (t) {
    case Type::List: return PropertiesPanel::tr("List");
    case Type::Number: return PropertiesPanel::tr("Number");
    case Type::Checkbox: return PropertiesPanel::tr("Checkbox");
    case Type::Date: return PropertiesPanel::tr("Date");
    case Type::DateTime: return PropertiesPanel::tr("Date & time");
    case Type::Raw: return PropertiesPanel::tr("Edit the source to change this value");
    default: return PropertiesPanel::tr("Text");
    }
}

// Pills wrap like words; the last item (the input) takes the rest of its line.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override
    {
        while (QLayoutItem *i = takeAt(0))
            delete i;
    }
    void addItem(QLayoutItem *item) override { m_items << item; }
    void insertWidgetAt(int index, QWidget *w)
    {
        addChildWidget(w);
        m_items.insert(index, new QWidgetItem(w));
        invalidate();
    }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return place(QRect(0, 0, w, 0), false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize s;
        for (const QLayoutItem *i : m_items)
            s = s.expandedTo(i->minimumSize());
        return s;
    }
    void setGeometry(const QRect &r) override
    {
        QLayout::setGeometry(r);
        place(r, true);
    }

private:
    int place(const QRect &r, bool apply) const
    {
        const int gap = 4;
        int x = r.x(), y = r.y(), line = 0;
        for (QLayoutItem *item : m_items) {
            QSize s = item->sizeHint();
            const bool last = item == m_items.last();
            if (last)
                s.setWidth(item->minimumSize().width());
            if (x > r.x() && x + s.width() > r.right() + 1) {
                x = r.x();
                y += line + gap;
                line = 0;
            }
            if (last)
                s.setWidth(qMax(s.width(), r.right() + 1 - x));
            if (apply)
                item->setGeometry(QRect(QPoint(x, y), s));
            x += s.width() + gap;
            line = qMax(line, s.height());
        }
        return y + line - r.y();
    }

    QList<QLayoutItem *> m_items;
};

class PillList : public QWidget
{
public:
    PillList(const QStringList &items, bool tags, QWidget *parent)
        : QWidget(parent)
        , m_tags(tags)
        , m_flow(new FlowLayout(this))
    {
        input = new QLineEdit;
        input->setMinimumWidth(48);
        m_flow->addWidget(input);
        for (const QString &item : items)
            addPill(item);
        m_items = items;
        connect(input, &QLineEdit::returnPressed, this, &PillList::take);
        connect(input, &QLineEdit::editingFinished, this, &PillList::take);
        input->installEventFilter(this);
    }

    std::function<void(const QStringList &)> changed;
    QLineEdit *input;

protected:
    bool eventFilter(QObject *obj, QEvent *e) override
    {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Backspace
            && input->text().isEmpty() && !m_pills.isEmpty()) {
            removePill(m_pills.last());
            return true;
        }
        return QWidget::eventFilter(obj, e);
    }

private:
    void take()
    {
        QStringList added;
        for (QString v : input->text().split(u',', Qt::SkipEmptyParts)) {
            v = v.trimmed();
            if (m_tags && v.startsWith(u'#'))
                v = v.mid(1);
            if (!v.isEmpty() && !m_items.contains(v) && !added.contains(v))
                added << v;
        }
        input->clear();
        if (added.isEmpty())
            return;
        for (const QString &v : std::as_const(added))
            addPill(v);
        m_items += added;
        if (changed)
            changed(m_items);
    }

    void addPill(const QString &text)
    {
        auto *pill = new QFrame;
        pill->setObjectName(u"pill"_s);
        auto *row = new QHBoxLayout(pill);
        row->setContentsMargins(8, 1, 2, 1);
        row->setSpacing(2);
        row->addWidget(new QLabel(text));
        auto *x = new QToolButton;
        x->setObjectName(u"pillRemove"_s);
        x->setIcon(Theme::icon(u"close"_s));
        x->setIconSize(QSize(10, 10));
        x->setFocusPolicy(Qt::NoFocus);
        x->setToolTip(PropertiesPanel::tr("Remove"));
        row->addWidget(x);
        connect(x, &QToolButton::clicked, this, [this, pill] { removePill(pill); });
        m_flow->insertWidgetAt(int(m_pills.size()), pill);
        m_pills << pill;
    }

    void removePill(QWidget *pill)
    {
        const qsizetype i = m_pills.indexOf(pill);
        if (i < 0)
            return;
        m_pills.removeAt(i);
        m_items.removeAt(i);
        m_flow->removeWidget(pill);
        pill->hide();
        pill->deleteLater();
        if (changed)
            changed(m_items);
    }

    bool m_tags;
    FlowLayout *m_flow;
    QList<QWidget *> m_pills;
    QStringList m_items;
};

// Drawn like the task checkboxes in the editor rather than the platform's.
class CheckBox : public QAbstractButton
{
public:
    CheckBox()
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override { return QSize(30, 28); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QRectF box(7, (height() - 16) / 2.0, 16, 16);
        if (hasFocus()) {
            p.setPen(QPen(Theme::colors().accent, 1.5));
            p.drawRoundedRect(box.adjusted(-3, -3, 3, 3), 5, 5);
        }
        if (isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.color(QPalette::Link));
            p.drawRoundedRect(box, 3, 3);
            p.setPen(QPen(pal.color(QPalette::Base), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(QList<QPointF>{box.topLeft() + QPointF(3.8, 8.3), box.topLeft() + QPointF(6.9, 11.2),
                                          box.topLeft() + QPointF(12.2, 5.1)});
        } else {
            p.setPen(QPen(pal.color(QPalette::PlaceholderText), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        }
    }
};

QLineEdit *dateEdit(const QString &value, bool withTime)
{
    auto *e = new QLineEdit(value);
    e->setMaximumWidth(withTime ? 240 : 200);
    e->setPlaceholderText(withTime ? u"YYYY-MM-DDTHH:mm"_s : u"YYYY-MM-DD"_s);
    e->setValidator(new QRegularExpressionValidator(
        QRegularExpression(withTime ? u"[0-9]{0,4}(-[0-9]{0,2}(-[0-9]{0,2}([T ][0-9]{0,2}(:[0-9]{0,2}(:[0-9]{0,2})?)?)?)?)?"_s
                                    : u"[0-9]{0,4}(-[0-9]{0,2}(-[0-9]{0,2})?)?"_s),
        e));
    QAction *pick = e->addAction(Theme::icon(u"calendar"_s), QLineEdit::TrailingPosition);
    pick->setToolTip(PropertiesPanel::tr("Pick a date"));
    QObject::connect(pick, &QAction::triggered, e, [e, withTime] {
        QMenu menu(e);
        auto *calendar = new QCalendarWidget;
        calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
        if (const QDate d = QDate::fromString(e->text().left(10), Qt::ISODate); d.isValid())
            calendar->setSelectedDate(d);
        auto *holder = new QWidgetAction(&menu);
        holder->setDefaultWidget(calendar);
        menu.addAction(holder);
        QObject::connect(calendar, &QCalendarWidget::clicked, &menu, [&menu, e, withTime](QDate d) {
            QString text = d.toString(Qt::ISODate);
            if (withTime)
                text += e->text().size() > 10 ? e->text().mid(10) : u"T00:00"_s;
            e->setText(text);
            menu.close();
            Q_EMIT e->editingFinished();
        });
        menu.exec(e->mapToGlobal(QPoint(0, e->height())));
    });
    return e;
}

} // namespace

PropertiesPanel::PropertiesPanel(QWidget *parent)
    : QFrame(parent)
    , m_header(new QToolButton)
    , m_add(new QToolButton)
{
    setObjectName(u"properties"_s);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 10);
    layout->setSpacing(2);

    m_header->setObjectName(u"propertiesHeader"_s);
    m_header->setText(tr("Properties"));
    m_header->setIcon(Theme::icon(u"chevron-down"_s));
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setFocusPolicy(Qt::NoFocus);
    connect(m_header, &QToolButton::clicked, this, [this] {
        m_collapsed = !m_collapsed;
        m_header->setIcon(Theme::icon(m_collapsed ? u"chevron-right"_s : u"chevron-down"_s));
        if (m_body)
            m_body->setVisible(!m_collapsed);
        m_add->setVisible(!m_collapsed);
    });
    layout->addWidget(m_header, 0, Qt::AlignLeft);

    m_add->setText(tr("Add property"));
    m_add->setIcon(Theme::icon(u"plus"_s));
    m_add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_add->setProperty("propRow", -1);
    m_add->installEventFilter(this);
    connect(m_add, &QToolButton::clicked, this, &PropertiesPanel::addProperty);
    layout->addWidget(m_add, 0, Qt::AlignLeft);
}

void PropertiesPanel::setSource(const QStringList &lines)
{
    if (lines == m_source && m_body)
        return;
    m_source = lines;
    m_props = Frontmatter::parse(lines);
    rebuild();
}

QStringList PropertiesPanel::joined() const
{
    QStringList out;
    for (const Property &p : m_props)
        out += p.lines;
    return out;
}

int PropertiesPanel::heightFor(int width) const
{
    QLayout *l = layout();
    const int h = l->hasHeightForWidth() ? l->totalHeightForWidth(width) : l->totalSizeHint().height();
    return qMax(h, l->totalMinimumSize().height());
}

bool PropertiesPanel::event(QEvent *e)
{
    const bool done = QFrame::event(e);
    if (e->type() == QEvent::LayoutRequest)
        QTimer::singleShot(0, this, &PropertiesPanel::heightChanged);
    return done;
}

void PropertiesPanel::rebuild(int focusRow, bool focusValue)
{
    if (m_body) {
        m_body->hide();
        m_body->setParent(nullptr);
        m_body->deleteLater();
    }
    m_rows.clear();
    m_newKey = nullptr;
    m_body = new QWidget;
    auto *grid = new QGridLayout(m_body);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);
    grid->setColumnStretch(2, 1);

    for (int i = 0; i < m_props.size(); ++i) {
        const Property &p = m_props[i];
        if (p.key.isEmpty())
            continue;
        const int r = int(m_rows.size());
        auto *icon = new QToolButton;
        icon->setIcon(Theme::icon(typeIcon(p)));
        icon->setFocusPolicy(Qt::NoFocus);
        icon->setToolTip(p.key == u"tags" ? tr("Tags") : p.key == u"aliases" ? tr("Aliases") : typeName(p.type));
        connect(icon, &QToolButton::clicked, this, [this, i, icon] { showRowMenu(i, icon->mapToGlobal(QPoint(0, icon->height()))); });

        auto *key = new QLineEdit(p.key);
        key->setObjectName(u"propertyKey"_s);
        key->setFixedWidth(keyWidth());
        connect(key, &QLineEdit::editingFinished, this, [this, i, key, original = p.key] {
            if (i < m_props.size() && m_props[i].key == original && key->isVisible())
                renameKey(i, key);
        });
        QWidget *value = valueEditor(i);
        connect(key, &QLineEdit::returnPressed, this, [this, r] {
            if (r < m_rows.size() && m_rows[r].value)
                m_rows[r].value->setFocus();
        });

        grid->addWidget(icon, r, 0, Qt::AlignTop);
        grid->addWidget(key, r, 1, Qt::AlignTop);
        grid->addWidget(value, r, 2);
        QWidget *focusTarget = value->focusProxy() ? value->focusProxy() : value;
        if (focusTarget->focusPolicy() == Qt::NoFocus)
            focusTarget = nullptr;
        m_rows.append({key, focusTarget});
        key->setProperty("propCol", 0);
        watch(key, r);
        if (focusTarget) {
            focusTarget->setProperty("propCol", 1);
            watch(focusTarget, r);
        }
    }
    static_cast<QVBoxLayout *>(layout())->insertWidget(1, m_body);
    m_body->setVisible(!m_collapsed);
    if (focusRow >= 0 && focusRow < m_rows.size()) {
        const Row &row = m_rows[focusRow];
        (focusValue && row.value ? row.value : static_cast<QWidget *>(row.key))->setFocus();
    }
}

QWidget *PropertiesPanel::valueEditor(int index)
{
    const Property &p = m_props[index];
    const QString key = p.key;
    // Edits made in widgets of an older rebuild must not land on a shifted index.
    auto current = [this, index, key] { return index < m_props.size() && m_props[index].key == key; };
    switch (p.type) {
    case Type::List: {
        auto *list = new PillList(p.items, key == u"tags", nullptr);
        list->setFocusProxy(list->input);
        list->changed = [this, index, current](const QStringList &items) {
            if (!current())
                return;
            m_props[index].items = items;
            commit(index);
        };
        return list;
    }
    case Type::Checkbox: {
        auto *box = new CheckBox;
        box->setObjectName(u"propertyCheck"_s);
        box->setChecked(p.value == u"true");
        connect(box, &QAbstractButton::toggled, this, [this, index, current](bool on) {
            if (!current())
                return;
            m_props[index].value = on ? u"true"_s : u"false"_s;
            commit(index);
        });
        return box;
    }
    case Type::Raw: {
        QStringList body = p.lines.mid(1);
        const QString first = p.lines.value(0).section(u':', 1).trimmed();
        if (!first.isEmpty())
            body.prepend(first);
        auto *label = new QLabel(body.join(u' ').simplified());
        label->setObjectName(u"propertyRaw"_s);
        label->setToolTip(typeName(Type::Raw));
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        return label;
    }
    default: {
        QLineEdit *edit = p.type == Type::Date || p.type == Type::DateTime ? dateEdit(p.value, p.type == Type::DateTime)
                                                                           : new QLineEdit(p.value);
        if (p.type == Type::Number)
            edit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"[-+]?[0-9]*([.][0-9]*)?"_s), edit));
        connect(edit, &QLineEdit::editingFinished, this, [this, index, current, edit] {
            if (!current() || edit->text() == m_props[index].value)
                return;
            const Type type = m_props[index].type;
            const QString v = edit->text().trimmed();
            const bool valid = v.isEmpty() || (type == Type::Date && Frontmatter::isDate(v))
                               || (type == Type::DateTime && Frontmatter::isDateTime(v))
                               || (type == Type::Number && Frontmatter::isNumber(v)) || type == Type::Text;
            if (!valid) {
                edit->setText(m_props[index].value);
                return;
            }
            m_props[index].value = type == Type::Text ? edit->text() : v;
            commit(index);
        });
        return edit;
    }
    }
}

void PropertiesPanel::commit(int index)
{
    m_props[index].lines = Frontmatter::serialize(m_props[index]);
    m_source = joined();
    Q_EMIT changed(m_source);
}

void PropertiesPanel::watch(QWidget *w, int row)
{
    w->setProperty("propRow", row);
    w->installEventFilter(this);
}

bool PropertiesPanel::eventFilter(QObject *obj, QEvent *e)
{
    // Qt only reports editingFinished for a changed text, so an untouched new row is dropped here.
    if (e->type() == QEvent::FocusOut && obj == m_newKey && m_newKey->text().trimmed().isEmpty()
        && static_cast<QFocusEvent *>(e)->reason() != Qt::PopupFocusReason
        && static_cast<QFocusEvent *>(e)->reason() != Qt::ActiveWindowFocusReason) {
        QTimer::singleShot(0, this, [this] {
            if (m_newKey && !m_newKey->hasFocus())
                commitNewKey();
        });
    }
    if (e->type() != QEvent::KeyPress)
        return QFrame::eventFilter(obj, e);
    const auto *k = static_cast<QKeyEvent *>(e);
    const int row = obj->property("propRow").toInt();
    const int col = obj->property("propCol").toInt();
    if (k->key() == Qt::Key_Escape) {
        Q_EMIT leave(true);
        return true;
    }
    if ((k->key() != Qt::Key_Up && k->key() != Qt::Key_Down) || k->modifiers() != Qt::NoModifier)
        return QFrame::eventFilter(obj, e);
    const bool down = k->key() == Qt::Key_Down;
    if (obj == m_add) {
        if (down)
            Q_EMIT leave(true);
        else
            focusRow(true);
        return true;
    }
    const int target = row + (down ? 1 : -1);
    if (target < 0)
        return true;
    if (target >= m_rows.size()) {
        if (m_newKey && obj != m_newKey)
            m_newKey->setFocus();
        else
            Q_EMIT leave(true);
        return true;
    }
    QWidget *w = col == 1 && m_rows[target].value ? m_rows[target].value : m_rows[target].key;
    w->setFocus();
    return true;
}

void PropertiesPanel::focusRow(bool last)
{
    if (m_collapsed) {
        m_add->setFocus();
        return;
    }
    if (m_rows.isEmpty()) {
        (m_newKey ? static_cast<QWidget *>(m_newKey) : m_add)->setFocus();
        return;
    }
    const Row &row = last ? m_rows.last() : m_rows.first();
    (row.value ? row.value : static_cast<QWidget *>(row.key))->setFocus();
}

void PropertiesPanel::addProperty()
{
    if (m_collapsed)
        m_header->click();
    if (m_newKey) {
        m_newKey->setFocus();
        return;
    }
    auto *grid = static_cast<QGridLayout *>(m_body->layout());
    const int r = int(m_rows.size());
    auto *icon = new QToolButton;
    icon->setIcon(Theme::icon(u"text"_s));
    icon->setFocusPolicy(Qt::NoFocus);
    m_newKey = new QLineEdit;
    m_newKey->setObjectName(u"propertyKey"_s);
    m_newKey->setFixedWidth(keyWidth());
    m_newKey->setPlaceholderText(tr("Name"));
    m_newKey->setProperty("propCol", 0);
    watch(m_newKey, r);
    QPointer<QLineEdit> edit = m_newKey;
    connect(m_newKey, &QLineEdit::editingFinished, this, [this, edit] {
        if (edit && edit == m_newKey)
            commitNewKey();
    });
    grid->addWidget(icon, r, 0, Qt::AlignTop);
    grid->addWidget(m_newKey, r, 1, Qt::AlignTop);
    icon->show();
    m_newKey->show();
    m_newKey->setFocus();
}

void PropertiesPanel::commitNewKey()
{
    const QString key = m_newKey->text().trimmed();
    bool exists = false;
    for (const Property &p : std::as_const(m_props))
        exists = exists || p.key == key;
    if (key.isEmpty() || exists) {
        const bool hadFocus = m_newKey->hasFocus();
        rebuild();
        bool any = false;
        for (const Property &p : std::as_const(m_props))
            any = any || !p.key.isEmpty();
        if (!any) {
            m_source.clear();
            Q_EMIT changed({});
        } else if (hadFocus) {
            focusRow(true);
        }
        return;
    }
    Property p;
    p.key = key;
    p.type = Frontmatter::fixedList(key) ? Type::List : Type::Text;
    m_props << p;
    commit(int(m_props.size()) - 1);
    rebuild(int(m_rows.size()), true);
}

void PropertiesPanel::renameKey(int index, QLineEdit *edit)
{
    const QString key = edit->text().trimmed();
    if (key == m_props[index].key)
        return;
    bool exists = false;
    for (const Property &p : std::as_const(m_props))
        exists = exists || p.key == key;
    if (key.isEmpty() || exists) {
        edit->setText(m_props[index].key);
        return;
    }
    m_props[index].key = key;
    commit(index);
}

void PropertiesPanel::showRowMenu(int index, const QPoint &globalPos)
{
    if (index >= m_props.size())
        return;
    const Property &p = m_props[index];
    QMenu menu(this);
    QMenu *types = menu.addMenu(tr("Property type"));
    types->setEnabled(p.type != Type::Raw && !Frontmatter::fixedList(p.key));
    for (Type t : {Type::Text, Type::List, Type::Number, Type::Checkbox, Type::Date, Type::DateTime}) {
        QAction *a = types->addAction(typeName(t).replace(u'&', u"&&"_s), this, [this, index, t] { setType(index, t); });
        a->setCheckable(true);
        a->setChecked(p.type == t);
    }
    menu.addSeparator();
    if (p.type == Type::Raw)
        menu.addAction(tr("Edit source"), this, &PropertiesPanel::editSource);
    menu.addAction(tr("Remove"), this, [this, index] { remove(index); });
    menu.exec(globalPos);
}

void PropertiesPanel::setType(int index, Type type)
{
    Property &p = m_props[index];
    if (p.type == type)
        return;
    const QString scalar = p.type == Type::List ? p.items.join(u", "_s) : p.value;
    p.value.clear();
    p.items.clear();
    switch (type) {
    case Type::List:
        if (p.type == Type::Text || p.type == Type::Raw)
            p.items = scalar.isEmpty() ? QStringList() : QStringList{scalar};
        else if (!scalar.isEmpty())
            p.items = {scalar};
        break;
    case Type::Number:
        p.value = Frontmatter::isNumber(scalar) ? scalar : QString();
        break;
    case Type::Checkbox:
        p.value = scalar == u"true" ? u"true"_s : u"false"_s;
        break;
    case Type::Date:
        p.value = Frontmatter::isDate(scalar.left(10)) ? scalar.left(10) : QString();
        break;
    case Type::DateTime:
        p.value = Frontmatter::isDateTime(scalar) ? scalar : Frontmatter::isDate(scalar) ? scalar + u"T00:00"_s : QString();
        break;
    default:
        p.value = scalar;
        break;
    }
    p.type = type;
    commit(index);
    rebuild();
}

void PropertiesPanel::remove(int index)
{
    m_props.removeAt(index);
    bool any = false;
    for (const Property &p : std::as_const(m_props))
        any = any || !p.key.isEmpty();
    m_source = any ? joined() : QStringList();
    Q_EMIT changed(m_source);
    rebuild();
}

void PropertiesPanel::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    menu.addAction(tr("Add property"), this, &PropertiesPanel::addProperty);
    menu.addAction(tr("Edit source"), this, &PropertiesPanel::editSource);
    menu.exec(e->globalPos());
}

// Line edits pass Return on to their parent; it must not reach the note text underneath.
void PropertiesPanel::keyPressEvent(QKeyEvent *e)
{
    e->accept();
}

int PropertiesPanel::keyWidth() const
{
    return qBound(96, width() / 4, 150);
}

void PropertiesPanel::resizeEvent(QResizeEvent *e)
{
    QFrame::resizeEvent(e);
    for (QLineEdit *key : findChildren<QLineEdit *>(u"propertyKey"_s))
        key->setFixedWidth(keyWidth());
}
