#include "RecoveryDialog.h"

#include "Vault.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
constexpr int PathRole = Qt::UserRole;
constexpr int FileRole = Qt::UserRole + 1;
}

RecoveryDialog::RecoveryDialog(Vault *vault, const QString &path, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_filter(new QLineEdit(this))
    , m_notes(new QListWidget(this))
    , m_versions(new QListWidget(this))
    , m_preview(new QPlainTextEdit(this))
    , m_restore(new QPushButton(tr("Restore"), this))
    , m_copy(new QPushButton(tr("Copy"), this))
{
    setWindowTitle(tr("File recovery"));
    resize(960, 560);
    m_filter->setPlaceholderText(tr("Find a note…"));
    m_filter->setClearButtonEnabled(true);
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_restore->setObjectName(u"primary"_s);
    m_restore->setEnabled(false);
    m_copy->setEnabled(false);

    auto *hint = new QLabel(tr("Serat keeps a copy of each note every few minutes while you work and before a note is "
                               "deleted. Restoring replaces the note's text; the text it had is kept as a version too."),
                            this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);

    auto *left = new QVBoxLayout;
    left->addWidget(m_filter);
    left->addWidget(m_notes, 1);
    auto *columns = new QHBoxLayout;
    columns->addLayout(left, 3);
    columns->addWidget(m_versions, 2);
    columns->addWidget(m_preview, 5);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(m_copy, QDialogButtonBox::ActionRole);
    buttons->addButton(m_restore, QDialogButtonBox::AcceptRole);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_filter, &QLineEdit::textChanged, this, &RecoveryDialog::fillNotes);
    connect(m_notes, &QListWidget::currentItemChanged, this, &RecoveryDialog::fillVersions);
    connect(m_versions, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        m_text = item ? Recovery::read({item->data(FileRole).toString(), {}, 0}) : QString();
        m_preview->setPlainText(m_text);
        m_restore->setEnabled(item);
        m_copy->setEnabled(item);
    });
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_text); });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *note = m_notes->currentItem()) {
            m_path = note->data(PathRole).toString();
            accept();
        }
    });

    fillNotes();
    for (int i = 0; i < m_notes->count(); ++i)
        if (m_notes->item(i)->data(PathRole).toString() == path)
            m_notes->setCurrentRow(i);
    if (!m_notes->currentItem() && m_notes->count())
        m_notes->setCurrentRow(0);
}

void RecoveryDialog::fillNotes()
{
    const QString current = m_notes->currentItem() ? m_notes->currentItem()->data(PathRole).toString() : QString();
    m_notes->clear();
    const QString filter = m_filter->text().trimmed();
    for (const QString &p : m_vault->recovery().paths()) {
        if (!filter.isEmpty() && !p.contains(filter, Qt::CaseInsensitive))
            continue;
        const bool gone = !m_vault->exists(p);
        auto *item = new QListWidgetItem(gone ? tr("%1 (deleted)").arg(p) : p, m_notes);
        item->setData(PathRole, p);
        if (gone)
            item->setForeground(palette().color(QPalette::PlaceholderText));
        if (p == current)
            m_notes->setCurrentItem(item);
    }
    if (m_notes->count() == 0)
        fillVersions();
}

void RecoveryDialog::fillVersions()
{
    m_versions->clear();
    const QListWidgetItem *note = m_notes->currentItem();
    if (!note)
        return;
    const QLocale locale;
    for (const Recovery::Version &v : m_vault->recovery().versions(note->data(PathRole).toString())) {
        auto *item = new QListWidgetItem(locale.toString(v.time, QLocale::ShortFormat) + u"   "_s + locale.formattedDataSize(v.size), m_versions);
        item->setData(FileRole, v.file);
    }
    m_versions->setCurrentRow(0);
}
