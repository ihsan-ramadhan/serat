#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>

struct Link {
    QString target;   // part before '#' and '|', as written
    QString heading;  // part after '#'
    QString alias;    // part after '|'
    bool embed = false;
    int line = 0;     // 0-based line in the note
    int start = 0;    // offset of the token ('!' or first '[') in the note text
    int length = 0;   // length of the whole token
};

struct Heading {
    int level = 1;
    QString text;
    int line = 0;
};

struct Note {
    QString path; // vault-relative, '/'-separated, with extension
    QString text;
    QList<Link> links;
    QList<Heading> headings;
    QStringList tags;          // without '#', from the frontmatter and the text
    QStringList aliases;
    QHash<QString, int> blocks; // "^id" at the end of a line -> that line
    QStringList properties;    // frontmatter keys
};

struct SearchHit {
    QString path;
    int line = -1; // -1 = matched by file name only
    QString text;
};

class Vault : public QObject
{
    Q_OBJECT
public:
    explicit Vault(const QString &root, QObject *parent = nullptr);

    QString root() const { return m_root; }
    QString name() const;
    QString absolutePath(const QString &rel) const;
    QString relativePath(const QString &abs) const;

    const QHash<QString, Note> &notes() const { return m_notes; }
    const Note *note(const QString &path) const;
    const QStringList &files() const { return m_files; }
    bool exists(const QString &path) const;
    bool isDir(const QString &path) const;

    // Obsidian's order: exact path, then file name preferring `from`'s folder, then shortest path.
    QString resolve(const QString &target, const QString &from) const;
    // Shortest link text that resolves back to `path`.
    QString linkText(const QString &path) const;
    QList<QPair<QString, Link>> backlinks(const QString &path) const;
    QList<SearchHit> search(const QString &query, int limit = 500, bool matchCase = false) const;

    QString uniquePath(const QString &wanted) const;
    QString createNote(const QString &path, const QString &text = {});
    bool createFolder(const QString &path);
    bool writeNote(const QString &path, const QString &text);
    QString readText(const QString &path) const;
    // Also rewrites links pointing into it; returns an error message, empty on success.
    QString rename(const QString &from, const QString &to);
    bool remove(const QString &path);

    static Note parse(const QString &path, const QString &text);
    static bool isNote(const QString &path) { return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive); }
    static bool isImage(const QString &path);
    static QString baseName(const QString &path);
    // Why `name` cannot be a file or folder name, or an empty string when it can.
    static QString nameProblem(const QString &name);
    // `name` with characters that cannot be in a file name replaced by '-'.
    static QString safeName(const QString &name);

public Q_SLOTS:
    void rescan();

Q_SIGNALS:
    void changed();
    void noteChangedOnDisk(const QString &path);
    void renamed(const QString &from, const QString &to);

private:
    void scanDir(const QString &rel, QStringList &files, QStringList &dirs) const;
    void rebuildNameIndex();
    QString linkTextIn(const QString &path, bool keepFullPath) const;
    qint64 fileStamp(const QString &path) const;

    QString m_root;
    QHash<QString, Note> m_notes;
    QHash<QString, qint64> m_stamps;              // mtime/size, skips re-reading unchanged notes
    QStringList m_files;
    QStringList m_dirs;
    QHash<QString, QString> m_lowerPath;          // lowercase path -> path
    QHash<QString, QStringList> m_byFileName;     // lowercase file name -> paths
    QFileSystemWatcher m_watcher;
    QTimer m_rescanTimer;
};
