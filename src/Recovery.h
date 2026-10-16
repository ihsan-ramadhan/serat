#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// Earlier versions of a vault's notes, kept in the app's data folder rather than in the vault.
class Recovery
{
public:
    struct Version {
        QString file;
        QDateTime time;
        qint64 size = 0;
    };

    explicit Recovery(const QString &vaultRoot);

    // Kept when the last one of `path` is older than the snapshot interval, or when `force`d.
    void snapshot(const QString &path, const QString &text, bool force = false);
    // Notes that have versions, most recently saved first.
    QStringList paths() const;
    QList<Version> versions(const QString &path) const; // newest first
    static QString read(const Version &version);
    // Keeps a renamed or moved note's versions with it.
    void moved(const QString &from, const QString &to);
    // Drops versions older than the keep period, except each note's newest.
    void prune();
    void clear();

    static int intervalMinutes();
    static int keepDays();

private:
    QString dirFor(const QString &path) const;

    QString m_dir;
};
