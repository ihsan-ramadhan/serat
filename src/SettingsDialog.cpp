#include "SettingsDialog.h"

#include "CommandPalette.h"
#include "Settings.h"
#include "Spelling.h"
#include "Theme.h"
#include "Vault.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>

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

SettingsDialog::SettingsDialog(Vault *vault, QMenuBar *menus, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_menus(menus)
    , m_sections(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
{
    setWindowTitle(tr("Settings"));
    resize(760, 520);
    setMinimumSize(520, 360);
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
    buildHotkeys();
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
    // Scrolls instead of squeezing rows over each other when the dialog is shorter than the page.
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pages->addWidget(scroll);
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

    if (!Spelling::supported()) {
        form->addRow(note(tr("This build of Serat has no spellchecker.")));
        return;
    }
    box(tr("Spellcheck"), u"spellcheck"_s, true, tr("Underline words the dictionaries do not know. Right-click one for suggestions."));
    auto *languages = new QListWidget;
    languages->setObjectName(u"spellLanguages"_s);
    const QStringList chosen = Spelling::instance().languages();
    QStringList names = Spelling::instance().available();
    std::stable_sort(names.begin(), names.end(), [&chosen](const QString &a, const QString &b) {
        return chosen.contains(a) && !chosen.contains(b);
    });
    for (const QString &name : names) {
        auto *item = new QListWidgetItem(u"%1  ·  %2"_s.arg(Spelling::displayName(name), name), languages);
        item->setData(Qt::UserRole, name);
        item->setCheckState(chosen.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    constexpr int visibleRows = 6;
    languages->ensurePolished();
    languages->setFixedHeight(languages->sizeHintForRow(0) * qBound(1, int(languages->count()), visibleRows) + 2 * languages->frameWidth());
    auto *languageField = new QWidget;
    auto *languageLayout = new QVBoxLayout(languageField);
    languageLayout->setContentsMargins({});
    if (languages->count() > visibleRows) {
        auto *filter = new QLineEdit;
        filter->setObjectName(u"spellLanguageFilter"_s);
        filter->setPlaceholderText(tr("Filter languages"));
        filter->setClearButtonEnabled(true);
        connect(filter, &QLineEdit::textChanged, languages, [languages](const QString &text) {
            for (int i = 0; i < languages->count(); ++i)
                languages->item(i)->setHidden(!languages->item(i)->text().contains(text.trimmed(), Qt::CaseInsensitive));
        });
        languageLayout->addWidget(filter);
    }
    languageLayout->addWidget(languages);
    connect(languages, &QListWidget::itemChanged, this, [this, languages] {
        QStringList names;
        for (int i = 0; i < languages->count(); ++i)
            if (languages->item(i)->checkState() == Qt::Checked)
                names << languages->item(i)->data(Qt::UserRole).toString();
        Settings::setApp(u"spellLanguages"_s, names);
        Q_EMIT changed();
    });
    form->addRow(tr("Languages"), languageField);
    form->addRow(note(tr("More languages: put Hunspell .aff and .dic files in %1.")
                          .arg(QDir::toNativeSeparators(Spelling::userDictionaryDir()))));
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
    auto folderEdit = [folders](const QString &text) {
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

    auto *interval = new QSpinBox;
    interval->setRange(1, 120);
    interval->setSuffix(tr(" min"));
    interval->setValue(Recovery::intervalMinutes());
    auto *keep = new QSpinBox;
    keep->setRange(1, 365);
    keep->setSuffix(tr(" days"));
    keep->setValue(Recovery::keepDays());
    auto *clear = new QPushButton(tr("Clear history"));
    form->addRow(tr("Save a version every"), interval);
    form->addRow(tr("Keep versions for"), keep);
    form->addRow(note(tr("Earlier versions of notes, for File > File recovery. They are kept outside the vault.")));
    form->addRow(QString(), clear);
    connect(interval, &QSpinBox::valueChanged, this, [](int v) { Settings::setApp(u"recoveryInterval"_s, v); });
    connect(keep, &QSpinBox::valueChanged, this, [](int v) { Settings::setApp(u"recoveryDays"_s, v); });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clear history"), tr("Delete every saved version of the notes in this vault?")) == QMessageBox::Yes)
            m_vault->recovery().clear();
    });

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

    auto *language = new QComboBox;
    language->addItem(tr("System default"), u"system"_s);
    language->addItem(u"English"_s, u"en"_s);
    language->addItem(u"Bahasa Indonesia"_s, u"id"_s);
    language->setCurrentIndex(qMax(0, language->findData(Settings::app(u"language"_s, u"system"_s))));
    connect(language, &QComboBox::currentIndexChanged, this, [language] { Settings::setApp(u"language"_s, language->currentData()); });
    form->addRow(tr("Language"), language);
    form->addRow(note(tr("A new language shows after Serat restarts.")));

    auto fontChoice = [this](const QString &key, bool fixedPitch) {
        auto *combo = new QComboBox;
        combo->addItem(tr("Default"), QString());
        for (const QString &family : QFontDatabase::families())
            if (!QFontDatabase::isPrivateFamily(family) && (!fixedPitch || QFontDatabase::isFixedPitch(family)))
                combo->addItem(family, family);
        combo->setCurrentIndex(qMax(0, combo->findData(Settings::app(key).toString())));
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key] {
            Settings::setApp(key, combo->currentData());
            Q_EMIT changed();
        });
        return combo;
    };
    form->addRow(tr("Text font"), fontChoice(u"textFont"_s, false));
    form->addRow(tr("Monospace font"), fontChoice(u"monoFont"_s, true));
    auto *size = new QSpinBox;
    size->setRange(8, 36);
    size->setSuffix(tr(" pt"));
    size->setValue(qRound(Settings::app(u"textSize"_s, Settings::defaultTextSize()).toDouble()));
    connect(size, &QSpinBox::valueChanged, this, [this](int pt) {
        Settings::setApp(u"textSize"_s, pt);
        Q_EMIT changed();
    });
    form->addRow(tr("Font size"), size);
    form->addRow(note(tr("View > Zoom in and Zoom out (Ctrl+= and Ctrl+-) change the size on top of this.")));
}

void SettingsDialog::buildHotkeys()
{
    QFormLayout *form = addPage(tr("Hotkeys"));
    if (!m_menus)
        return;
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter commands…"));
    filter->setClearButtonEnabled(true);
    auto *list = new QTreeWidget;
    list->setObjectName(u"hotkeys"_s);
    list->setHeaderLabels({tr("Command"), tr("Hotkey")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setMinimumHeight(260);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list->header()->setStretchLastSection(false);
    auto *keys = new QKeySequenceEdit;
    keys->setObjectName(u"hotkeyEdit"_s);
    keys->setMaximumSequenceLength(1);
    keys->setClearButtonEnabled(true);
    auto *reset = new QPushButton(tr("Restore default"));
    auto *row = new QHBoxLayout;
    row->addWidget(keys, 1);
    row->addWidget(reset);
    form->addRow(filter);
    form->addRow(list);
    form->addRow(tr("Hotkey"), row);
    form->addRow(note(tr("Select a command, then press the keys for it. The clear button removes its hotkey.")));

    QList<QAction *> actions;
    for (const auto &[name, action] : CommandPalette::commands(m_menus)) {
        if (action->objectName().isEmpty())
            continue;
        auto *item = new QTreeWidgetItem(list, {name});
        item->setData(0, Qt::UserRole, int(actions.size()));
        actions << action;
    }
    auto actionOf = [actions](QTreeWidgetItem *item) {
        return item ? actions.value(item->data(0, Qt::UserRole).toInt()) : nullptr;
    };
    auto refresh = [list, actions] {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = list->topLevelItem(i);
            item->setText(1, QKeySequence::listToString(actions[item->data(0, Qt::UserRole).toInt()]->shortcuts(),
                                                        QKeySequence::NativeText));
        }
    };
    auto store = [](QAction *a, const QList<QKeySequence> &shortcuts) {
        QSettings s;
        const QString text = QKeySequence::listToString(shortcuts, QKeySequence::PortableText);
        if (text == a->property("defaultKeys").toString())
            s.remove(u"shortcuts/"_s + a->objectName());
        else
            s.setValue(u"shortcuts/"_s + a->objectName(), text);
        a->setShortcuts(shortcuts);
    };
    auto showCurrent = [list, keys, reset, actionOf] {
        QAction *a = actionOf(list->currentItem());
        const QSignalBlocker block(keys);
        keys->setKeySequence(a ? a->shortcut() : QKeySequence());
        keys->setEnabled(a);
        reset->setEnabled(a && QKeySequence::listToString(a->shortcuts(), QKeySequence::PortableText)
                                   != a->property("defaultKeys").toString());
    };
    auto assign = [this, list, keys, actions, actionOf, store, refresh, showCurrent] {
        QAction *a = actionOf(list->currentItem());
        if (!a)
            return;
        const QKeySequence key = keys->keySequence();
        if (!key.isEmpty() && a->shortcuts().contains(key))
            return;
        if (!key.isEmpty()) {
            for (QAction *other : actions) {
                if (other == a || !other->shortcuts().contains(key))
                    continue;
                const QString name = other->text().remove(u'&').remove(u'…');
                if (QMessageBox::question(this, tr("Hotkey in use"),
                                          tr("%1 is used by “%2”. Use it here instead?").arg(key.toString(QKeySequence::NativeText), name))
                    != QMessageBox::Yes) {
                    showCurrent();
                    return;
                }
                QList<QKeySequence> rest = other->shortcuts();
                rest.removeAll(key);
                store(other, rest);
            }
        }
        store(a, key.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{key});
        refresh();
        showCurrent();
    };
    connect(keys, &QKeySequenceEdit::editingFinished, this, assign);
    connect(keys, &QKeySequenceEdit::keySequenceChanged, this, [keys, assign](const QKeySequence &key) {
        if (key.isEmpty() && keys->isEnabled())
            assign();
    });
    connect(reset, &QPushButton::clicked, this, [list, actionOf, store, refresh, showCurrent] {
        if (QAction *a = actionOf(list->currentItem())) {
            store(a, QKeySequence::listFromString(a->property("defaultKeys").toString(), QKeySequence::PortableText));
            refresh();
            showCurrent();
        }
    });
    connect(list, &QTreeWidget::currentItemChanged, this, showCurrent);
    connect(filter, &QLineEdit::textChanged, this, [list](const QString &text) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = list->topLevelItem(i);
            item->setHidden(!item->text(0).contains(text, Qt::CaseInsensitive) && !item->text(1).contains(text, Qt::CaseInsensitive));
        }
    });
    refresh();
    showCurrent();
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
