#pragma once

#include <QDateTime>
#include <QFont>
#include <QVariant>

class QAction;
class Vault;

// Per-vault values fall back to the vault's own .obsidian/*.json (read, never written).
namespace Settings {

QVariant app(const QString &key, const QVariant &fallback = {});
void setApp(const QString &key, const QVariant &value);

// Settings > Appearance, plus the zoom from View > Zoom in.
QFont textFont(double zoom = 0);
double defaultTextSize();
QString monoFamily();
// Hotkeys set in Settings, stored by the action's object name, override the ones it was created with.
void applyShortcuts(const QList<QAction *> &actions);

QString vaultGroup(const Vault &vault);
QVariant vault(const Vault &vault, const QString &key, const QString &jsonFile = {}, const QString &jsonKey = {},
               const QVariant &fallback = {});
void setVault(const Vault &vault, const QString &key, const QVariant &value);

// "/" vault root, "./" next to the note, "./name" a folder beside the note, "name" a fixed folder.
QString attachmentSpec(const Vault &vault);
QString attachmentDir(const Vault &vault, const QString &notePath);
// "root", "current" (the open note's folder) or "folder" (newNoteFolder()).
QString newNoteLocation(const Vault &vault);
QString newNoteFolder(const Vault &vault);
QString newNoteDir(const Vault &vault, const QString &currentPath);
// Settings > Files, or else the vault's own "confirm file deletion".
bool confirmDelete(const Vault &vault);

struct Daily {
    QString folder;
    QString format;   // moment.js style, e.g. YYYY-MM-DD
    QString templatePath;
};
Daily daily(const Vault &vault);

struct Templates {
    QString folder;
    QString dateFormat;
    QString timeFormat;
};
Templates templates(const Vault &vault);

QString formatMoment(const QDateTime &time, const QString &format);
// {{title}}, {{date}}, {{time}}, {{date:FORMAT}} and {{time:FORMAT}}.
QString applyTemplate(const QString &text, const QString &title, const Templates &formats, const QDateTime &now);

}
