#include "Graph.h"

#include "Vault.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QVarLengthArray>
#include <QWheelEvent>

#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

constexpr double linkLength = 60;
constexpr double charge = 400;
constexpr double reach = 300; // notes further apart do not push, so loose notes stay in sight

// Barnes-Hut: far groups of notes push as one, so a step costs n log n instead of n squared.
class QuadTree
{
public:
    explicit QuadTree(const QList<QPointF> &points)
        : m_points(points)
    {
        double left = points[0].x(), right = left, top = points[0].y(), bottom = top;
        for (const QPointF &p : points) {
            left = qMin(left, p.x());
            right = qMax(right, p.x());
            top = qMin(top, p.y());
            bottom = qMax(bottom, p.y());
        }
        const double half = qMax(right - left, bottom - top) / 2 + 1;
        m_cells.reserve(points.size() * 2);
        m_cells.emplace_back(QPointF((left + right) / 2, (top + bottom) / 2), half);
        for (int i = 0; i < points.size(); ++i)
            insert(0, i, 0);
    }

    QPointF forceOn(int i) const
    {
        QPointF f;
        QVarLengthArray<int, 64> stack{0};
        while (!stack.isEmpty()) {
            const Cell &c = m_cells[stack.last()];
            stack.removeLast();
            if (c.mass == 0 || c.body == i)
                continue;
            const QPointF d = m_points[i] - c.centre / c.mass;
            const double dist2 = qMax(25.0, QPointF::dotProduct(d, d));
            if (dist2 > (reach + 2 * c.half) * (reach + 2 * c.half))
                continue;
            if (c.leaf() || 4 * c.half * c.half < 0.64 * dist2) {
                if (dist2 > reach * reach)
                    continue;
                f += d * (charge * c.mass / dist2);
                continue;
            }
            for (const int k : c.child)
                if (k >= 0)
                    stack.append(k);
        }
        return f;
    }

private:
    struct Cell {
        Cell(QPointF m, double h)
            : middle(m)
            , half(h)
        {
        }
        QPointF middle;
        double half;
        double mass = 0;
        QPointF centre; // sum of positions; divided by mass when used
        int body = -1;  // the one note in a leaf
        int child[4] = {-1, -1, -1, -1};
        bool leaf() const { return child[0] < 0 && child[1] < 0 && child[2] < 0 && child[3] < 0; }
    };

    void insert(int cell, int body, int depth)
    {
        const QPointF p = m_points[body];
        Cell *c = &m_cells[cell];
        c->mass += 1;
        c->centre += p;
        if (c->mass == 1) {
            c->body = body;
            return;
        }
        if (depth > 40) // notes on the same spot share the leaf
            return;
        if (c->leaf() && c->body >= 0) {
            const int old = std::exchange(c->body, -1);
            sink(cell, old, depth);
            c = &m_cells[cell];
        }
        c->body = -1;
        sink(cell, body, depth);
    }

    void sink(int cell, int body, int depth)
    {
        const QPointF p = m_points[body];
        const Cell c = m_cells[cell];
        const int q = (p.x() >= c.middle.x() ? 1 : 0) + (p.y() >= c.middle.y() ? 2 : 0);
        if (c.child[q] < 0) {
            const double h = c.half / 2;
            m_cells.emplace_back(c.middle + QPointF(q & 1 ? h : -h, q & 2 ? h : -h), h);
            m_cells[cell].child[q] = int(m_cells.size()) - 1;
        }
        insert(m_cells[cell].child[q], body, depth + 1);
    }

    const QList<QPointF> &m_points;
    std::vector<Cell> m_cells;
};

QList<QPointF> repulsion(const QList<QPointF> &points)
{
    QList<QPointF> force(points.size());
    if (points.isEmpty())
        return force;
    const QuadTree tree(points);
    for (int i = 0; i < points.size(); ++i)
        force[i] = tree.forceOn(i);
    return force;
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

}

GraphView::GraphView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"graph"_s);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_tick.setInterval(16);
    connect(&m_tick, &QTimer::timeout, this, &GraphView::step);
}

void GraphView::setScope(const Vault *vault, const QString &center, int depth)
{
    if (vault != m_vault)
        m_nodes.clear();
    m_vault = vault;
    m_center = center;
    m_depth = depth;
    refresh();
}

void GraphView::setFilter(const QString &text)
{
    m_filter = text.trimmed();
    refresh();
}

void GraphView::refresh()
{
    QHash<QString, QPointF> was;
    for (const Node &n : std::as_const(m_nodes))
        was.insert(n.path, n.pos);
    m_nodes.clear();
    m_edges.clear();
    m_neighbours.clear();
    m_hover = m_dragged = -1;

    QStringList paths;
    if (m_vault && m_depth == 0) {
        for (auto it = m_vault->notes().cbegin(); it != m_vault->notes().cend(); ++it)
            if (m_filter.isEmpty() || it.key().contains(m_filter, Qt::CaseInsensitive))
                paths << it.key();
    } else if (m_vault && m_vault->note(m_center)) {
        QSet<QString> seen{m_center};
        QStringList ring{m_center};
        for (int d = 0; d < m_depth; ++d) {
            QStringList next;
            for (const QString &p : std::as_const(ring)) {
                QStringList near = m_vault->linkTargets(p).values();
                for (const auto &[source, link] : m_vault->backlinks(p))
                    near << source;
                for (const QString &q : std::as_const(near))
                    if (Vault::isNote(q) && !seen.contains(q)) {
                        seen.insert(q);
                        next << q;
                    }
            }
            ring = next;
        }
        paths = seen.values();
    }
    paths.sort();

    QHash<QString, int> index;
    int placed = 0;
    for (const QString &p : std::as_const(paths)) {
        Node n;
        n.path = p;
        n.label = Vault::baseName(p);
        if (const auto it = was.constFind(p); it != was.constEnd()) {
            n.pos = *it;
        } else if (p == m_center) {
            n.pos = {};
        } else {
            // A sunflower spiral: an even, overlap-free start that is the same every time.
            const double r = 20 * std::sqrt(placed + 0.5), a = placed * 2.39996;
            n.pos = {r * std::cos(a), r * std::sin(a)};
            ++placed;
        }
        index.insert(p, int(m_nodes.size()));
        m_nodes << n;
    }
    m_neighbours.resize(m_nodes.size());
    QSet<std::pair<int, int>> pairs;
    for (int i = 0; i < m_nodes.size(); ++i) {
        for (const QString &t : m_vault->linkTargets(m_nodes[i].path)) {
            const int j = index.value(t, -1);
            if (j < 0 || j == i)
                continue;
            const std::pair<int, int> e{qMin(i, j), qMax(i, j)};
            if (pairs.contains(e))
                continue;
            pairs.insert(e);
            m_edges << e;
            m_neighbours[i] << j;
            m_neighbours[j] << i;
        }
    }
    for (int i = 0; i < m_nodes.size(); ++i)
        m_nodes[i].degree = int(m_neighbours[i].size());
    m_cache = {};
    if (was.isEmpty())
        m_fitted = false;
    reheat(placed > 0 ? 1.0 : 0.3);
    update();
}

void GraphView::reheat(double alpha)
{
    m_alpha = qMax(m_alpha, alpha);
    if (isVisible() && !m_nodes.isEmpty())
        m_tick.start();
}

void GraphView::step()
{
    const int n = int(m_nodes.size());
    QList<QPointF> pos(n);
    for (int i = 0; i < n; ++i)
        pos[i] = m_nodes[i].pos;
    QList<QPointF> force = repulsion(pos);
    for (const auto &[a, b] : std::as_const(m_edges)) {
        const QPointF d = m_nodes[b].pos - m_nodes[a].pos;
        const double dist = qMax(1.0, std::hypot(d.x(), d.y()));
        const QPointF pull = d / dist * ((dist - linkLength) * 0.08);
        force[a] += pull;
        force[b] -= pull;
    }
    // A pull to the middle keeps notes without links nearby; gentler in big vaults so they have room.
    const double gravity = 0.012 * qMin(1.0, std::sqrt(200.0 / n));
    for (int i = 0; i < n; ++i) {
        Node &node = m_nodes[i];
        if (i == m_dragged)
            continue;
        force[i] -= node.pos * gravity;
        node.velocity = (node.velocity + force[i] * m_alpha) * 0.6;
        // Capped, so a crowded start cannot fling notes away.
        if (const double speed = std::hypot(node.velocity.x(), node.velocity.y()); speed > 30)
            node.velocity *= 30 / speed;
        node.pos += node.velocity;
    }
    m_alpha *= 0.975;
    m_cache = {};
    if (m_alpha < 0.01 || !isVisible())
        m_tick.stop();
    if (!m_fitted)
        fit();
    // Drawing thousands of links costs more than a step; big graphs show every few steps while they settle.
    if (n < 2000 || ++m_frame % 8 == 0 || !m_tick.isActive() || m_dragged >= 0)
        update();
}

void GraphView::fit()
{
    if (m_nodes.isEmpty())
        return;
    double left = m_nodes[0].pos.x(), right = left, top = m_nodes[0].pos.y(), bottom = top;
    for (const Node &n : std::as_const(m_nodes)) {
        left = qMin(left, n.pos.x());
        right = qMax(right, n.pos.x());
        top = qMin(top, n.pos.y());
        bottom = qMax(bottom, n.pos.y());
    }
    const QRectF box = QRectF(QPointF(left, top), QPointF(right, bottom)).adjusted(-40, -40, 40, 40);
    m_zoom = qBound(0.05, qMin(width() / box.width(), height() / box.height()), 2.0);
    m_pan = -box.center() * m_zoom;
}

QPointF GraphView::toWidget(const QPointF &p) const
{
    return QPointF(width() / 2.0, height() / 2.0) + m_pan + p * m_zoom;
}

QPointF GraphView::toGraph(const QPointF &p) const
{
    return (p - QPointF(width() / 2.0, height() / 2.0) - m_pan) / m_zoom;
}

QPointF GraphView::nodePosition(const QString &path) const
{
    for (const Node &n : m_nodes)
        if (n.path == path)
            return toWidget(n.pos);
    return {-1, -1};
}

double GraphView::radius(const Node &n) const
{
    return qMax(2.5, (4.5 + qMin(std::sqrt(double(n.degree)), 6.0) * 1.5) * m_zoom);
}

int GraphView::nodeAt(const QPointF &at) const
{
    int best = -1;
    double bestDist = 0;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const double d = QLineF(toWidget(m_nodes[i].pos), at).length();
        if (d <= radius(m_nodes[i]) + 4 && (best < 0 || d < bestDist)) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

void GraphView::drawScene(QPainter &p, bool links, const QSet<int> &only, bool labels) const
{
    const QColor base = palette().color(QPalette::Base), text = palette().color(QPalette::Text);
    const QColor accent = palette().color(QPalette::Link), node = mix(base, text, 0.55);
    const QRectF view = QRectF(rect()).adjusted(-20, -20, 20, 20);
    QList<QPointF> at(m_nodes.size());
    for (int i = 0; i < m_nodes.size(); ++i)
        at[i] = toWidget(m_nodes[i].pos);
    p.setRenderHint(QPainter::Antialiasing, m_nodes.size() < 3000 || !only.isEmpty());
    // Batched: one call per colour and size is what keeps a vault of thousands of notes smooth.
    if (links) {
        QList<QLineF> lines;
        lines.reserve(m_edges.size());
        for (const auto &[a, b] : std::as_const(m_edges))
            if (view.contains(at[a]) || view.contains(at[b]))
                lines << QLineF(at[a], at[b]);
        p.setPen(QPen(mix(base, text, 0.25), 1));
        p.drawLines(lines);
    }
    QHash<std::pair<QRgb, int>, QList<QPointF>> dots;
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (!view.contains(at[i]) || (!only.isEmpty() && !only.contains(i)))
            continue;
        const QColor c = i == m_hover || m_nodes[i].path == m_center ? accent : node;
        dots[{c.rgb(), qRound(radius(m_nodes[i]) * 2)}] << at[i];
    }
    for (auto it = dots.cbegin(); it != dots.cend(); ++it) {
        p.setPen(QPen(QColor::fromRgb(it.key().first), it.key().second, Qt::SolidLine, Qt::RoundCap));
        p.drawPoints(it->constData(), int(it->size()));
    }
    if (!labels)
        return;
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(small);
    p.setPen(text);
    const QFontMetrics fm(small);
    for (int i = 0; i < m_nodes.size(); ++i) {
        if ((!only.isEmpty() && !only.contains(i)) || !rect().adjusted(-100, -20, 100, 20).contains(at[i].toPoint()))
            continue;
        const double top = at[i].y() + radius(m_nodes[i]) + 2;
        p.drawText(QRectF(at[i].x() - 100, top, 200, fm.height()), Qt::AlignHCenter | Qt::AlignTop,
                   fm.elidedText(m_nodes[i].label, Qt::ElideRight, 180));
    }
}

void GraphView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QColor base = palette().color(QPalette::Base);
    p.fillRect(rect(), base);
    if (m_nodes.isEmpty()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, m_depth ? tr("Open a note to see its links.") : tr("No notes to show."));
        return;
    }
    const bool allLabels = m_zoom > 0.9 || m_nodes.size() <= 40;
    // Thousands of long links take a while to draw; big graphs leave them out while moving.
    if (m_nodes.size() > 2000 && (m_tick.isActive() || m_mouseDown)) {
        drawScene(p, false, {}, false);
    } else {
        const qreal dpr = devicePixelRatioF();
        if (m_cache.isNull()) {
            m_cache = QPixmap(size() * dpr);
            m_cache.setDevicePixelRatio(dpr);
            m_cache.fill(base);
            QPainter cache(&m_cache);
            drawScene(cache, true, {}, allLabels);
        }
        p.drawPixmap(0, 0, m_cache);
    }
    if (m_hover < 0)
        return;
    // The note under the mouse and its neighbours stand out over a veil.
    QColor veil = base;
    veil.setAlpha(190);
    p.fillRect(rect(), veil);
    QSet<int> lit{m_hover};
    QList<QLineF> lines;
    for (const int j : m_neighbours[m_hover]) {
        lit.insert(j);
        lines << QLineF(toWidget(m_nodes[m_hover].pos), toWidget(m_nodes[j].pos));
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(palette().color(QPalette::Link), 1.4));
    p.drawLines(lines);
    drawScene(p, false, lit, true);
}

void GraphView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    m_cache = {};
    if (!m_fitted)
        fit();
}

void GraphView::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::FontChange)
        m_cache = {};
}

void GraphView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_alpha >= 0.01 && !m_nodes.isEmpty())
        m_tick.start();
}

void GraphView::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_tick.stop();
}

void GraphView::wheelEvent(QWheelEvent *e)
{
    const QPointF at = e->position();
    const QPointF anchor = toGraph(at);
    m_zoom = qBound(0.02, m_zoom * std::pow(1.0015, e->angleDelta().y()), 6.0);
    m_pan = at - QPointF(width() / 2.0, height() / 2.0) - anchor * m_zoom;
    m_fitted = true;
    m_cache = {};
    update();
}

void GraphView::mousePressEvent(QMouseEvent *e)
{
    m_pressAt = m_lastMouse = e->position();
    m_moved = false;
    m_mouseDown = true;
    m_dragged = e->button() == Qt::LeftButton ? nodeAt(e->position()) : -1;
}

void GraphView::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF at = e->position();
    if (e->buttons() & Qt::LeftButton) {
        if ((at - m_pressAt).manhattanLength() > 3)
            m_moved = true;
        if (m_dragged >= 0) {
            m_nodes[m_dragged].pos = toGraph(at);
            m_nodes[m_dragged].velocity = {};
            reheat(0.3);
        } else {
            m_pan += at - m_lastMouse;
            m_fitted = true;
        }
        m_lastMouse = at;
        m_cache = {};
        update();
        return;
    }
    const int hover = nodeAt(at);
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(hover >= 0 ? m_nodes[hover].path : QString());
        update();
    }
}

void GraphView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_moved && m_dragged >= 0)
        Q_EMIT openRequested(m_nodes[m_dragged].path, e->modifiers() & Qt::ControlModifier);
    m_dragged = -1;
    m_mouseDown = false;
    update();
}

void GraphView::leaveEvent(QEvent *e)
{
    QWidget::leaveEvent(e);
    if (m_hover >= 0) {
        m_hover = -1;
        update();
    }
}
