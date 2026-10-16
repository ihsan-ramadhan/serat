#pragma once

#include <QImage>
#include <QPixmap>
#include <QWidget>

#include <functional>

class Reader;
class QLabel;

// A widget drawn over Markdown lines in live preview; the editor reveals the lines when the cursor goes there.
class EmbedBlock : public QWidget
{
public:
    explicit EmbedBlock(QWidget *parent);

    QString key;  // what is shown, so a rescan can reuse the widget
    QString path; // vault-relative, for images and notes
    bool centered = false;
    std::function<void(const QPoint &)> menu;
    std::function<void()> clicked;

    virtual QSize sizeFor(int maxWidth) const = 0;

protected:
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
};

// An image file, rendered math or a diagram.
class ImageEmbed : public EmbedBlock
{
public:
    using EmbedBlock::EmbedBlock;
    void setFile(const QString &file, const QSize &wanted);
    void setImage(const QImage &image); // drawn at its own size, only ever scaled down
    bool isNull() const { return m_image.isNull(); }
    QSize sizeFor(int maxWidth) const override;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QString m_file;
    QImage m_image;
    QSize m_natural, m_wanted;
    QPixmap m_scaled;
};

// Another note (or one of its sections) shown read-only, with its name to open it.
class NoteEmbed : public EmbedBlock
{
public:
    explicit NoteEmbed(QWidget *parent);
    void setContent(const QString &title, const QString &html, const QFont &font, const QString &styleSheet);
    QSize sizeFor(int maxWidth) const override;
    std::function<void(bool newTab)> open;
    Reader *reader() const { return m_reader; }

private:
    QLabel *m_title;
    Reader *m_reader;
};
