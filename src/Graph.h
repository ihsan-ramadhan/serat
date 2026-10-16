#pragma once

#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QPointF>
#include <QTimer>
#include <QWidget>

class QPainter;
class Vault;

// Notes as dots and links as lines, laid out by a small force simulation.
class GraphView : public QWidget
{
    Q_OBJECT
public:
    explicit GraphView(QWidget *parent = nullptr);

    // Depth 0 shows the whole vault; otherwise the notes within that many links of `center`.
    void setScope(const Vault *vault, const QString &center, int depth = 0);
    void setFilter(const QString &text);
    // Rebuilds from the vault, keeping the places of notes already shown.
    void refresh();
    int nodeCount() const { return int(m_nodes.size()); }
    QPointF nodePosition(const QString &path) const; // in widget coordinates
    bool settled() const { return !m_tick.isActive(); }

Q_SIGNALS:
    void openRequested(const QString &path, bool newTab);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    struct Node {
        QString path;
        QString label;
        QPointF pos;
        QPointF velocity;
        int degree = 0;
    };
    void step();
    void drawScene(QPainter &p, bool links, const QSet<int> &only, bool labels) const;
    void reheat(double alpha);
    void fit();
    int nodeAt(const QPointF &widgetPos) const;
    QPointF toWidget(const QPointF &p) const;
    QPointF toGraph(const QPointF &p) const;
    double radius(const Node &n) const;

    const Vault *m_vault = nullptr;
    QString m_center;
    int m_depth = 0;
    QString m_filter;
    QList<Node> m_nodes;
    QList<std::pair<int, int>> m_edges;
    QList<QList<int>> m_neighbours;
    QTimer m_tick;
    double m_alpha = 0;
    QPointF m_pan;
    double m_zoom = 1;
    bool m_fitted = false;
    int m_frame = 0;
    int m_hover = -1;
    int m_dragged = -1;
    QPointF m_pressAt;
    QPointF m_lastMouse;
    bool m_moved = false;
    bool m_mouseDown = false;
    mutable QPixmap m_cache; // the still picture, so hovering does not redraw every link
};
