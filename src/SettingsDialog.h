#pragma once

#include <QDialog>

class QFormLayout;
class QListWidget;
class QMenuBar;
class QStackedWidget;
class Vault;

// Changes apply as they are made; changed() tells the window to pick them up.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(Vault *vault, QMenuBar *menus, QWidget *parent = nullptr);

Q_SIGNALS:
    void changed();

private:
    QFormLayout *addPage(const QString &name);
    void buildEditor();
    void buildFiles();
    void buildAppearance();
    void buildDaily();
    void buildTemplates();
    void buildHotkeys();

    Vault *m_vault;
    QMenuBar *m_menus;
    QListWidget *m_sections;
    QStackedWidget *m_pages;
};
