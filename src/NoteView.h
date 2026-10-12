#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

class Editor;
class QLabel;
class QLineEdit;
class QScrollArea;
class QStackedWidget;
class Reader;
class Vault;

// One tab: a note (editor + reading view) or an image.
class NoteView : public QWidget
{
    Q_OBJECT
public:
    enum Mode { Edit, Read };

    explicit NoteView(Vault *vault, QWidget *parent = nullptr);

    QString path() const { return m_path; }
    bool isNote() const;
    void open(const QString &path, bool pushHistory = true);
    bool save();
    void reloadFromDisk();
    void pathRenamed(const QString &from, const QString &to);
    void resetTitle();

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    void goBack();
    void goForward();

    void jumpToHeading(const QString &heading);
    void jumpToLine(int line);
    void showFind();
    void focusTitle();
    void focusContent();
    void applyFont(const QFont &font);
    void setLinkCandidates(const QStringList &names);
    void refreshRender();
    QString statsText() const;

Q_SIGNALS:
    void linkRequested(const QString &target, bool newTab);
    void renameRequested(const QString &path, const QString &newBaseName);
    void pathChanged();
    void statsChanged();
    void modeChanged();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void load(const QString &path);
    QString editorText() const;
    void handleHref(const QString &href, bool newTab);
    void render();
    void toggleTask(int utf8Offset);
    void find(bool backward);
    void updateMargins();
    void scaleImage();

    Vault *m_vault;
    QString m_path;
    QString m_diskText;
    bool m_crlf = false;
    bool m_loading = false;
    Mode m_mode = Edit;
    QStringList m_back, m_forward;
    QStringList m_codeBlocks;
    QFont m_font;

    QWidget *m_titleBar;
    QLineEdit *m_title;
    QStackedWidget *m_stack;
    Editor *m_editor;
    Reader *m_reader;
    QScrollArea *m_imageArea;
    QLabel *m_image;
    QPixmap m_pixmap;
    QWidget *m_findBar;
    QLineEdit *m_findEdit;
    QTimer m_saveTimer;
    QTimer m_renderTimer;
};
