#pragma once

#include <QTextBrowser>

class Reader : public QTextBrowser
{
    Q_OBJECT
public:
    explicit Reader(QWidget *parent = nullptr);
    int contentWidth() const;

Q_SIGNALS:
    void contentWidthChanged();
    void openRequested(const QString &href, bool newTab);
    void imageRequested(const QString &localFile, bool newTab);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    QVariant loadResource(int type, const QUrl &name) override;

private:
    int m_lastWidth = 0;
};
