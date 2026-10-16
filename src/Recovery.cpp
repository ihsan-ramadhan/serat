#include "Recovery.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

QString hashOf(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

constexpr auto TimeFormat = "yyyyMMdd-HHmmss-zzz";

} // namespace

Recovery::Recovery(const QString &vaultRoot)
    : m_dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/recovery/"_s + hashOf(vaultRoot))
{
}

int Recovery::intervalMinutes()
{
    return qBound(1, QSettings().value(u"recoveryInterval"_s, 5).toInt(), 120);
}

int Recovery::keepDays()
{
    return qBound(1, QSettings().value(u"recoveryDays"_s, 7).toInt(), 365);
}

// One folder per note, named by a hash of its path; the path itself is stored inside.
QString Recovery::dirFor(const QString &path) const
{
    return m_dir + u'/' + hashOf(path);
}

void Recovery::snapshot(const QString &path, const QString &text, bool force)
{
    if (path.isEmpty())
        return;
    const QString dir = dirFor(path);
    const QList<Version> existing = versions(path);
    if (!existing.isEmpty()) {
        const Version &newest = existing.first();
        if (read(newest) == text)
            return;
        if (!force && newest.time.secsTo(QDateTime::currentDateTime()) < intervalMinutes() * 60)
            return;
    }
    if (!QDir().mkpath(dir))
        return;
    QFile name(dir + u"/path"_s);
    if (!name.exists() && name.open(QIODevice::WriteOnly))
        name.write(path.toUtf8());
    QSaveFile f(dir + u'/' + QDateTime::currentDateTime().toString(QLatin1String(TimeFormat)) + u".md"_s);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text.toUtf8());
        f.commit();
    }
}

QStringList Recovery::paths() const
{
    QList<QPair<QDateTime, QString>> found;
    for (const QFileInfo &d : QDir(m_dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile name(d.filePath() + u"/path"_s);
        if (!name.open(QIODevice::ReadOnly))
            continue;
        const QString path = QString::fromUtf8(name.readAll());
        const QList<Version> v = versions(path);
        if (!v.isEmpty())
            found.append({v.first().time, path});
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList out;
    for (const auto &[time, path] : std::as_const(found))
        out << path;
    return out;
}

QList<Recovery::Version> Recovery::versions(const QString &path) const
{
    QList<Version> out;
    const QDir dir(dirFor(path));
    for (const QFileInfo &f : dir.entryInfoList({u"*.md"_s}, QDir::Files, QDir::Name | QDir::Reversed)) {
        const QDateTime time = QDateTime::fromString(f.completeBaseName(), QLatin1String(TimeFormat));
        if (time.isValid())
            out.append({f.filePath(), time, f.size()});
    }
    return out;
}

QString Recovery::read(const Version &version)
{
    QFile f(version.file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void Recovery::moved(const QString &from, const QString &to)
{
    QDir dir(m_dir);
    if (!dir.exists(hashOf(from)) || dir.exists(hashOf(to)) || !dir.rename(hashOf(from), hashOf(to)))
        return;
    QFile name(dirFor(to) + u"/path"_s);
    if (name.open(QIODevice::WriteOnly | QIODevice::Truncate))
        name.write(to.toUtf8());
}

void Recovery::prune()
{
    const QDateTime limit = QDateTime::currentDateTime().addDays(-keepDays());
    for (const QFileInfo &d : QDir(m_dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QFileInfoList files = QDir(d.filePath()).entryInfoList({u"*.md"_s}, QDir::Files, QDir::Name | QDir::Reversed);
        for (qsizetype i = 1; i < files.size(); ++i)
            if (QDateTime::fromString(files[i].completeBaseName(), QLatin1String(TimeFormat)) < limit)
                QFile::remove(files[i].filePath());
    }
}

void Recovery::clear()
{
    QDir(m_dir).removeRecursively();
}
