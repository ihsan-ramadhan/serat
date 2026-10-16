#pragma once

#include <QTextBrowser>

class Reader : public QTextBrowser
{
    Q_OBJECT
public:
    explicit Reader(QWidget *parent = nullptr);
    int contentWidth() const;
    // Draws math, diagrams and icons at this pixel ratio instead of the screen's, for print.
    void setResourceScale(qreal scale) { m_scale = scale; }

Q_SIGNALS:
    void contentWidthChanged();
    void openRequested(const QString &href, bool newTab);
    void imageRequested(const QString &localFile, bool newTab);
    void hoverLink(const QString &href, const QPoint &globalPos);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    QVariant loadResource(int type, const QUrl &name) override;

private:
    int m_lastWidth = 0;
    qreal m_scale = 0;
    QString m_hover;
};

// A note shown in a small window while Ctrl is held over a link to it.
class PreviewPopup : public QFrame
{
public:
    explicit PreviewPopup(QWidget *parent);
    void popup(const QString &html, const QString &anchor, const QPoint &globalPos);
    Reader *reader() const { return m_reader; }

protected:
    void leaveEvent(QEvent *e) override;

private:
    Reader *m_reader;
};
