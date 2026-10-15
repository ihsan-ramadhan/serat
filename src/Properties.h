#pragma once

#include "Frontmatter.h"

#include <QFrame>

class QGridLayout;
class QLineEdit;
class QToolButton;

// Frontmatter shown as editable rows in place of the YAML lines.
class PropertiesPanel : public QFrame
{
    Q_OBJECT
public:
    explicit PropertiesPanel(QWidget *parent = nullptr);

    // The lines between the two "---" markers.
    void setSource(const QStringList &lines);
    int heightFor(int width) const;
    void addProperty();
    void focusRow(bool last);

Q_SIGNALS:
    void changed(const QStringList &lines); // empty when the last property was removed
    void leave(bool down);
    void editSource();
    void heightChanged();

protected:
    bool event(QEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    struct Row {
        QLineEdit *key = nullptr;
        QWidget *value = nullptr; // takes focus for Up/Down
    };
    void rebuild(int focusRow = -1, bool focusValue = true);
    QWidget *valueEditor(int index);
    void commit(int index);
    void showRowMenu(int index, const QPoint &globalPos);
    void setType(int index, Frontmatter::Type type);
    void remove(int index);
    void renameKey(int index, QLineEdit *edit);
    void commitNewKey();
    void watch(QWidget *w, int row);
    QStringList joined() const;
    int keyWidth() const;

    QList<Frontmatter::Property> m_props;
    QStringList m_source;
    QToolButton *m_header;
    QToolButton *m_add;
    QWidget *m_body = nullptr;
    QList<Row> m_rows;
    QLineEdit *m_newKey = nullptr;
    bool m_collapsed = false;
};
