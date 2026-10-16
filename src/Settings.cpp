#include "Settings.h"

#include "Vault.h"

#include <QAction>
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>

using namespace Qt::StringLiterals;

namespace Settings {

namespace {

QString dirOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

QString cleanFolder(QString folder)
{
    folder = folder.trimmed();
    while (folder.startsWith(u'/'))
        folder.remove(0, 1);
    while (folder.endsWith(u'/'))
        folder.chop(1);
    return folder;
}

} // namespace

QVariant app(const QString &key, const QVariant &fallback)
{
    return QSettings().value(key, fallback);
}

void setApp(const QString &key, const QVariant &value)
{
    QSettings().setValue(key, value);
}

double defaultTextSize()
{
    return qMax(12.0, QGuiApplication::font().pointSizeF() + 1.5);
}

QFont textFont(double zoom)
{
    QFont f = QGuiApplication::font();
    if (const QString family = app(u"textFont"_s).toString(); !family.isEmpty())
        f.setFamilies({family});
    const double size = app(u"textSize"_s, 0.0).toDouble();
    f.setPointSizeF((size > 0 ? size : defaultTextSize()) + zoom);
    return f;
}

QString monoFamily()
{
    const QString family = app(u"monoFont"_s).toString();
    return family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : family;
}

void applyShortcuts(const QList<QAction *> &actions)
{
    QSettings s;
    s.beginGroup(u"shortcuts"_s);
    for (QAction *a : actions) {
        if (a->objectName().isEmpty())
            continue;
        if (!a->property("defaultKeys").isValid())
            a->setProperty("defaultKeys", QKeySequence::listToString(a->shortcuts(), QKeySequence::PortableText));
        const QString keys = s.contains(a->objectName()) ? s.value(a->objectName()).toString() : a->property("defaultKeys").toString();
        a->setShortcuts(QKeySequence::listFromString(keys, QKeySequence::PortableText));
    }
}

QString vaultGroup(const Vault &vault)
{
    return u"vault-"_s + QString::fromLatin1(QCryptographicHash::hash(vault.root().toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

QVariant vault(const Vault &vault, const QString &key, const QString &jsonFile, const QString &jsonKey, const QVariant &fallback)
{
    QSettings settings;
    settings.beginGroup(vaultGroup(vault));
    if (settings.contains(key))
        return settings.value(key);
    if (!jsonFile.isEmpty()) {
        QFile f(vault.root() + u"/.obsidian/"_s + jsonFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonValue v = QJsonDocument::fromJson(f.readAll()).object().value(jsonKey);
            if (!v.isUndefined() && !v.isNull() && !(v.isString() && v.toString().isEmpty()))
                return v.toVariant();
        }
    }
    return fallback;
}

void setVault(const Vault &vault, const QString &key, const QVariant &value)
{
    QSettings settings;
    settings.beginGroup(vaultGroup(vault));
    settings.setValue(key, value);
}

QString attachmentSpec(const Vault &v)
{
    return vault(v, u"attachmentFolder"_s, u"app.json"_s, u"attachmentFolderPath"_s, u"/"_s).toString().trimmed();
}

QString attachmentDir(const Vault &v, const QString &notePath)
{
    const QString spec = attachmentSpec(v);
    if (spec == u"/" || spec.isEmpty())
        return {};
    if (spec == u"./" || spec == u".")
        return dirOf(notePath);
    if (spec.startsWith(u"./"_s)) {
        const QString base = dirOf(notePath);
        const QString sub = cleanFolder(spec.mid(2));
        return base.isEmpty() ? sub : base + u'/' + sub;
    }
    return cleanFolder(spec);
}

QString newNoteLocation(const Vault &v)
{
    return vault(v, u"newNoteLocation"_s, u"app.json"_s, u"newFileLocation"_s, u"root"_s).toString();
}

QString newNoteFolder(const Vault &v)
{
    return cleanFolder(vault(v, u"newNoteFolder"_s, u"app.json"_s, u"newFileFolderPath"_s).toString());
}

QString newNoteDir(const Vault &v, const QString &currentPath)
{
    const QString where = newNoteLocation(v);
    if (where == u"current")
        return dirOf(currentPath);
    if (where == u"folder")
        return newNoteFolder(v);
    return {};
}

bool confirmDelete(const Vault &v)
{
    if (QSettings().contains(u"confirmDelete"_s))
        return QSettings().value(u"confirmDelete"_s).toBool();
    return vault(v, u"confirmDelete"_s, u"app.json"_s, u"promptDelete"_s, true).toBool();
}

Daily daily(const Vault &v)
{
    Daily d;
    d.folder = cleanFolder(vault(v, u"dailyFolder"_s, u"daily-notes.json"_s, u"folder"_s).toString());
    d.format = vault(v, u"dailyFormat"_s, u"daily-notes.json"_s, u"format"_s, u"YYYY-MM-DD"_s).toString();
    d.templatePath = vault(v, u"dailyTemplate"_s, u"daily-notes.json"_s, u"template"_s).toString().trimmed();
    return d;
}

Templates templates(const Vault &v)
{
    Templates t;
    t.folder = cleanFolder(vault(v, u"templateFolder"_s, u"templates.json"_s, u"folder"_s).toString());
    t.dateFormat = vault(v, u"templateDateFormat"_s, u"templates.json"_s, u"dateFormat"_s, u"YYYY-MM-DD"_s).toString();
    t.timeFormat = vault(v, u"templateTimeFormat"_s, u"templates.json"_s, u"timeFormat"_s, u"HH:mm"_s).toString();
    return t;
}

// The moment.js tokens people put in note names and templates; [text] is copied as is.
QString formatMoment(const QDateTime &time, const QString &format)
{
    static const QRegularExpression token(
        u"\\[([^\\]]*)\\]|YYYY|YY|MMMM|MMM|MM|M|Do|DDDD|DD|D|dddd|ddd|dd|d|HH|H|hh|h|mm|m|ss|s|A|a|ww|w|gggg|Q|X|x"_s);
    const QLocale locale = QLocale::system();
    const QDate date = time.date();
    const QTime clock = time.time();
    const int week = date.weekNumber();
    auto pad = [](int n, int width) { return QString::number(n).rightJustified(width, u'0'); };
    QString out;
    qsizetype last = 0;
    for (auto it = token.globalMatch(format); it.hasNext();) {
        const auto m = it.next();
        out += format.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString t = m.captured(0);
        if (t.startsWith(u'['))
            out += m.captured(1);
        else if (t == u"YYYY" || t == u"gggg")
            out += pad(date.year(), 4);
        else if (t == u"YY")
            out += pad(date.year() % 100, 2);
        else if (t == u"MMMM")
            out += locale.standaloneMonthName(date.month());
        else if (t == u"MMM")
            out += locale.standaloneMonthName(date.month(), QLocale::ShortFormat);
        else if (t == u"MM")
            out += pad(date.month(), 2);
        else if (t == u"M")
            out += QString::number(date.month());
        else if (t == u"Do") {
            const int d = date.day();
            const char *suffix = (d % 100 >= 11 && d % 100 <= 13) ? "th" : d % 10 == 1 ? "st" : d % 10 == 2 ? "nd" : d % 10 == 3 ? "rd" : "th";
            out += QString::number(d) + QLatin1String(suffix);
        } else if (t == u"DDDD")
            out += pad(date.dayOfYear(), 3);
        else if (t == u"DD")
            out += pad(date.day(), 2);
        else if (t == u"D")
            out += QString::number(date.day());
        else if (t == u"dddd")
            out += locale.standaloneDayName(date.dayOfWeek());
        else if (t == u"ddd")
            out += locale.standaloneDayName(date.dayOfWeek(), QLocale::ShortFormat);
        else if (t == u"dd")
            out += locale.standaloneDayName(date.dayOfWeek(), QLocale::NarrowFormat);
        else if (t == u"d")
            out += QString::number(date.dayOfWeek() % 7);
        else if (t == u"HH")
            out += pad(clock.hour(), 2);
        else if (t == u"H")
            out += QString::number(clock.hour());
        else if (t == u"hh")
            out += pad(clock.hour() % 12 == 0 ? 12 : clock.hour() % 12, 2);
        else if (t == u"h")
            out += QString::number(clock.hour() % 12 == 0 ? 12 : clock.hour() % 12);
        else if (t == u"mm")
            out += pad(clock.minute(), 2);
        else if (t == u"m")
            out += QString::number(clock.minute());
        else if (t == u"ss")
            out += pad(clock.second(), 2);
        else if (t == u"s")
            out += QString::number(clock.second());
        else if (t == u"A")
            out += clock.hour() < 12 ? u"AM"_s : u"PM"_s;
        else if (t == u"a")
            out += clock.hour() < 12 ? u"am"_s : u"pm"_s;
        else if (t == u"ww")
            out += pad(week, 2);
        else if (t == u"w")
            out += QString::number(week);
        else if (t == u"Q")
            out += QString::number((date.month() - 1) / 3 + 1);
        else if (t == u"X")
            out += QString::number(time.toSecsSinceEpoch());
        else if (t == u"x")
            out += QString::number(time.toMSecsSinceEpoch());
    }
    return out + format.mid(last);
}

QString applyTemplate(const QString &text, const QString &title, const Templates &formats, const QDateTime &now)
{
    static const QRegularExpression field(u"\\{\\{\\s*(title|date|time)\\s*(?::([^}]*))?\\}\\}"_s);
    QString out;
    qsizetype last = 0;
    for (auto it = field.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString name = m.captured(1);
        const QString format = m.captured(2).trimmed();
        if (name == u"title")
            out += title;
        else
            out += formatMoment(now, !format.isEmpty() ? format : name == u"date" ? formats.dateFormat : formats.timeFormat);
    }
    return out + text.mid(last);
}

}
