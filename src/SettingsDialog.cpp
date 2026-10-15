#include "SettingsDialog.h"

#include "Settings.h"
#include "Theme.h"
#include "Vault.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>

using namespace Qt::StringLiterals;

namespace {

QLabel *note(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(Vault *vault, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_sections(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
{
    setWindowTitle(tr("Settings"));
    resize(760, 520);
    m_sections->setObjectName(u"settingsSections"_s);
    m_sections->setFixedWidth(180);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *body = new QHBoxLayout;
    body->addWidget(m_sections);
    body->addWidget(m_pages, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);
    connect(m_sections, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    buildEditor();
    buildFiles();
    buildAppearance();
    buildDaily();
    buildTemplates();
    m_sections->setCurrentRow(0);
}

QFormLayout *SettingsDialog::addPage(const QString &name)
{
    m_sections->addItem(name);
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setContentsMargins(16, 4, 8, 8);
    form->setVerticalSpacing(10);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_pages->addWidget(page);
    return form;
}

void SettingsDialog::buildEditor()
{
    QFormLayout *form = addPage(tr("Editor"));
    auto box = [this, form](const QString &label, const QString &key, bool fallback, const QString &help = {}) {
        auto *check = new QCheckBox(label);
        check->setChecked(Settings::app(key, fallback).toBool());
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) {
            Settings::setApp(key, on);
            Q_EMIT changed();
        });
        form->addRow(check);
        if (!help.isEmpty())
            form->addRow(note(help));
    };
    box(tr("Live preview"), u"livePreview"_s, true, tr("Hide Markdown symbols except where the cursor is."));
    box(tr("Readable line length"), u"readableWidth"_s, true, tr("Keep lines to a comfortable width on wide windows."));
    box(tr("Auto pair brackets"), u"autoPairBrackets"_s, true, tr("Typing ( { or \" adds the closing half."));
    box(tr("Auto pair Markdown syntax"), u"autoPairMarkdown"_s, true,
        tr("With text selected, typing * _ ~ = or ` wraps it. A backtick on its own gets a closing one."));
    box(tr("Fold headings and lists"), u"foldGutter"_s, true, tr("Show the fold arrows beside headings, lists and callouts."));
    box(tr("Indentation guides"), u"indentGuides"_s, true, tr("Draw lines that connect nested list items."));
}

void SettingsDialog::buildFiles()
{
    QFormLayout *form = addPage(tr("Files and links"));
    if (!m_vault) {
        form->addRow(note(tr("Open a vault to change these.")));
        return;
    }
    QStringList folders;
    for (const QString &f : m_vault->files())
        if (const QString dir = f.section(u'/', 0, -2); !dir.isEmpty() && !folders.contains(dir))
            folders << dir;
    folders.sort(Qt::CaseInsensitive);
    auto folderEdit = [this, folders](const QString &text) {
        auto *edit = new QLineEdit(text);
        auto *completer = new QCompleter(folders, edit);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        edit->setCompleter(completer);
        return edit;
    };

    auto *where = new QComboBox;
    where->addItem(tr("Vault folder"), u"root"_s);
    where->addItem(tr("Same folder as current file"), u"current"_s);
    where->addItem(tr("In the folder specified below"), u"folder"_s);
    where->setCurrentIndex(qMax(0, where->findData(Settings::newNoteLocation(*m_vault))));
    QLineEdit *noteFolder = folderEdit(Settings::newNoteFolder(*m_vault));
    noteFolder->setEnabled(where->currentData().toString() == u"folder");
    form->addRow(tr("Default location for new notes"), where);
    form->addRow(tr("Folder for new notes"), noteFolder);
    connect(where, &QComboBox::currentIndexChanged, this, [this, where, noteFolder] {
        Settings::setVault(*m_vault, u"newNoteLocation"_s, where->currentData());
        noteFolder->setEnabled(where->currentData().toString() == u"folder");
    });
    connect(noteFolder, &QLineEdit::editingFinished, this,
            [this, noteFolder] { Settings::setVault(*m_vault, u"newNoteFolder"_s, noteFolder->text().trimmed()); });

    const QString spec = Settings::attachmentSpec(*m_vault);
    auto *attach = new QComboBox;
    attach->addItem(tr("Vault folder"), u"root"_s);
    attach->addItem(tr("Same folder as current file"), u"same"_s);
    attach->addItem(tr("In subfolder under current folder"), u"sub"_s);
    attach->addItem(tr("In the folder specified below"), u"folder"_s);
    const QString mode = spec == u"/" || spec.isEmpty() ? u"root"_s
                         : spec == u"./" || spec == u"."  ? u"same"_s
                         : spec.startsWith(u"./"_s)       ? u"sub"_s
                                                          : u"folder"_s;
    attach->setCurrentIndex(attach->findData(mode));
    QLineEdit *attachFolder = folderEdit(mode == u"sub" ? spec.mid(2) : mode == u"folder" ? spec : QString());
    attachFolder->setEnabled(mode == u"sub" || mode == u"folder");
    form->addRow(tr("Default location for new attachments"), attach);
    form->addRow(tr("Attachment folder"), attachFolder);
    form->addRow(note(tr("Pasted images and files dropped in from outside the vault are saved here.")));
    auto saveAttach = [this, attach, attachFolder] {
        const QString m = attach->currentData().toString();
        const QString folder = attachFolder->text().trimmed();
        attachFolder->setEnabled(m == u"sub" || m == u"folder");
        const QString value = m == u"root" ? u"/"_s : m == u"same" ? u"./"_s : m == u"sub" ? u"./"_s + folder : folder;
        Settings::setVault(*m_vault, u"attachmentFolder"_s, value);
    };
    connect(attach, &QComboBox::currentIndexChanged, this, saveAttach);
    connect(attachFolder, &QLineEdit::editingFinished, this, saveAttach);

    auto *confirm = new QCheckBox(tr("Confirm file deletion"));
    confirm->setChecked(Settings::confirmDelete(*m_vault));
    connect(confirm, &QCheckBox::toggled, this, [](bool on) { Settings::setApp(u"confirmDelete"_s, on); });
    form->addRow(confirm);
}

void SettingsDialog::buildAppearance()
{
    QFormLayout *form = addPage(tr("Appearance"));
    auto *theme = new QComboBox;
    theme->addItem(tr("Follow the system"), u"system"_s);
    theme->addItem(tr("Light"), u"light"_s);
    theme->addItem(tr("Dark"), u"dark"_s);
    theme->setCurrentIndex(qMax(0, theme->findData(Theme::preference())));
    connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] {
        Theme::setPreference(theme->currentData().toString());
        Q_EMIT changed();
    });
    form->addRow(tr("Theme"), theme);
    form->addRow(note(tr("Text size: View > Zoom in and Zoom out (Ctrl+= and Ctrl+-).")));
}

void SettingsDialog::buildDaily()
{
    QFormLayout *form = addPage(tr("Daily notes"));
    if (!m_vault) {
        form->addRow(note(tr("Open a vault to change these.")));
        return;
    }
    const Settings::Daily d = Settings::daily(*m_vault);
    auto *format = new QLineEdit(d.format);
    auto *sample = note({});
    auto showSample = [format, sample] {
        sample->setText(tr("Today's note is named: %1").arg(Settings::formatMoment(QDateTime::currentDateTime(), format->text())));
    };
    showSample();
    auto *folder = new QLineEdit(d.folder);
    auto *templ = new QLineEdit(d.templatePath);
    templ->setPlaceholderText(tr("Templates/Daily"));
    auto *startup = new QCheckBox(tr("Open daily note on startup"));
    startup->setChecked(Settings::vault(*m_vault, u"dailyOnStartup"_s, {}, {}, false).toBool());
    form->addRow(tr("Date format"), format);
    form->addRow(sample);
    form->addRow(note(tr("YYYY year, MM month, DD day, ddd weekday; text in [brackets] is kept. A / makes folders.")));
    form->addRow(tr("New file location"), folder);
    form->addRow(tr("Template file location"), templ);
    form->addRow(startup);
    connect(format, &QLineEdit::textChanged, this, showSample);
    connect(format, &QLineEdit::editingFinished, this,
            [this, format] { Settings::setVault(*m_vault, u"dailyFormat"_s, format->text().trimmed()); });
    connect(folder, &QLineEdit::editingFinished, this,
            [this, folder] { Settings::setVault(*m_vault, u"dailyFolder"_s, folder->text().trimmed()); });
    connect(templ, &QLineEdit::editingFinished, this,
            [this, templ] { Settings::setVault(*m_vault, u"dailyTemplate"_s, templ->text().trimmed()); });
    connect(startup, &QCheckBox::toggled, this, [this](bool on) { Settings::setVault(*m_vault, u"dailyOnStartup"_s, on); });
}

void SettingsDialog::buildTemplates()
{
    QFormLayout *form = addPage(tr("Templates"));
    if (!m_vault) {
        form->addRow(note(tr("Open a vault to change these.")));
        return;
    }
    const Settings::Templates t = Settings::templates(*m_vault);
    auto *folder = new QLineEdit(t.folder);
    folder->setPlaceholderText(tr("Templates"));
    auto *date = new QLineEdit(t.dateFormat);
    auto *time = new QLineEdit(t.timeFormat);
    auto *sample = note({});
    auto showSample = [date, time, sample] {
        const QDateTime now = QDateTime::currentDateTime();
        sample->setText(tr("{{date}} gives %1, {{time}} gives %2")
                            .arg(Settings::formatMoment(now, date->text()), Settings::formatMoment(now, time->text())));
    };
    showSample();
    form->addRow(tr("Template folder location"), folder);
    form->addRow(tr("Date format"), date);
    form->addRow(tr("Time format"), time);
    form->addRow(sample);
    form->addRow(note(tr("Insert template (in the Insert menu) copies a note from this folder into the current one, "
                         "filling in {{title}}, {{date}} and {{time}}.")));
    connect(date, &QLineEdit::textChanged, this, showSample);
    connect(time, &QLineEdit::textChanged, this, showSample);
    connect(folder, &QLineEdit::editingFinished, this,
            [this, folder] { Settings::setVault(*m_vault, u"templateFolder"_s, folder->text().trimmed()); });
    connect(date, &QLineEdit::editingFinished, this,
            [this, date] { Settings::setVault(*m_vault, u"templateDateFormat"_s, date->text().trimmed()); });
    connect(time, &QLineEdit::editingFinished, this,
            [this, time] { Settings::setVault(*m_vault, u"templateTimeFormat"_s, time->text().trimmed()); });
}
