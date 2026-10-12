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

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    int m_lastWidth = 0;
};
