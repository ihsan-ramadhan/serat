#include "Vault.h"

#include "Frontmatter.h"
#include "SearchQuery.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

QString dirOf(const QString &path)
{
    const int slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

QString stripMd(const QString &path)
{
    return Vault::isNote(path) ? path.chopped(3) : path;
}

bool lessPath(const QString &a, const QString &b)
{
    return QString::compare(a, b, Qt::CaseInsensitive) < 0;
}

} // namespace

Vault::Vault(const QString &root, QObject *parent)
    : QObject(parent)
    , m_root(QDir(root).absolutePath())
{
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(250);
    connect(&m_rescanTimer, &QTimer::timeout, this, &Vault::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_rescanTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_rescanTimer, qOverload<>(&QTimer::start));
    rescan();
}

QString Vault::name() const
{
    return QFileInfo(m_root).fileName();
}

QString Vault::absolutePath(const QString &rel) const
{
    return rel.isEmpty() ? m_root : m_root + u'/' + rel;
}

QString Vault::relativePath(const QString &abs) const
{
    return QDir(m_root).relativeFilePath(abs);
}

const Note *Vault::note(const QString &path) const
{
    const auto it = m_notes.constFind(path);
    return it == m_notes.constEnd() ? nullptr : &*it;
}

bool Vault::exists(const QString &path) const
{
    return QFileInfo::exists(absolutePath(path));
}

bool Vault::isDir(const QString &path) const
{
    return QFileInfo(absolutePath(path)).isDir();
}

bool Vault::isImage(const QString &path)
{
    static const QStringList exts = {u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"webp"_s, u"bmp"_s, u"svg"_s};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

QString Vault::baseName(const QString &path)
{
    return stripMd(path.section(u'/', -1));
}

void Vault::scanDir(const QString &rel, QStringList &files, QStringList &dirs) const
{
    const QDir dir(absolutePath(rel));
    const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &fi : entries) {
        if (fi.fileName().startsWith(u'.'))
            continue;
        const QString path = rel.isEmpty() ? fi.fileName() : rel + u'/' + fi.fileName();
        if (fi.isDir()) {
            if (fi.isSymLink())
                continue;
            dirs << path;
            scanDir(path, files, dirs);
        } else {
            files << path;
        }
    }
}

void Vault::rescan()
{
    QStringList files, dirs;
    scanDir({}, files, dirs);
    std::sort(files.begin(), files.end(), lessPath);
    std::sort(dirs.begin(), dirs.end(), lessPath);

    QHash<QString, Note> notes;
    QStringList changedOnDisk;
    for (const QString &f : std::as_const(files)) {
        if (!isNote(f))
            continue;
        const auto old = m_notes.constFind(f);
        const qint64 stamp = fileStamp(f);
        if (old != m_notes.constEnd() && m_stamps.value(f) == stamp) {
            notes.insert(f, *old);
            continue;
        }
        m_stamps.insert(f, stamp);
        const QString text = readText(f);
        if (old != m_notes.constEnd() && old->text == text) {
            notes.insert(f, *old);
            continue;
        }
        notes.insert(f, parse(f, text));
        if (old != m_notes.constEnd())
            changedOnDisk << f;
    }
    m_files = files;
    m_dirs = dirs;
    m_notes = notes;
    rebuildNameIndex();

    QStringList wanted{m_root};
    for (const QString &d : std::as_const(m_dirs))
        wanted << absolutePath(d);
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it)
        wanted << absolutePath(it.key());
    const QSet<QString> want(wanted.cbegin(), wanted.cend());
    QStringList watched = m_watcher.files() + m_watcher.directories();
    QStringList stale;
    for (const QString &p : std::as_const(watched))
        if (!want.contains(p))
            stale << p;
    if (!stale.isEmpty())
        m_watcher.removePaths(stale);
    const QSet<QString> have(watched.cbegin(), watched.cend());
    QStringList fresh;
    for (const QString &p : std::as_const(wanted))
        if (!have.contains(p))
            fresh << p;
    if (!fresh.isEmpty())
        m_watcher.addPaths(fresh);

    for (const QString &p : std::as_const(changedOnDisk))
        Q_EMIT noteChangedOnDisk(p);
    Q_EMIT changed();
}

void Vault::rebuildNameIndex()
{
    m_lowerPath.clear();
    m_byFileName.clear();
    for (const QString &f : std::as_const(m_files)) {
        m_lowerPath.insert(f.toLower(), f);
        m_byFileName[f.section(u'/', -1).toLower()].append(f);
    }
}

qint64 Vault::fileStamp(const QString &path) const
{
    const QFileInfo fi(absolutePath(path));
    return fi.lastModified().toMSecsSinceEpoch() * 1000003 + fi.size();
}

QString Vault::readText(const QString &path) const
{
    QFile f(absolutePath(path));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

Note Vault::parse(const QString &path, const QString &text)
{
    Note n;
    n.path = path;
    n.text = text;

    static const QRegularExpression trailingHashes(u"\\s+#+\\s*$"_s);
    bool inFence = false;
    bool inFrontmatter = false;
    QChar fenceChar;
    int fenceLen = 0;
    int lineNo = 0;
    qsizetype lineStart = 0;
    QStringList frontmatter;
    static const QRegularExpression blockId(u"\\s\\^([A-Za-z0-9-]+)\\s*$"_s);
    static const QRegularExpression tag(u"(?<![\\w#/&\\]])#([\\w/-]*[^\\W\\d_][\\w/-]*|[\\w/-]*_[\\w/-]*)"_s,
                                        QRegularExpression::UseUnicodePropertiesOption);
    const qsizetype len = text.size();

    while (lineStart <= len) {
        qsizetype nl = text.indexOf(u'\n', lineStart);
        if (nl < 0)
            nl = len;
        QStringView line = QStringView(text).mid(lineStart, nl - lineStart);
        if (line.endsWith(u'\r'))
            line.chop(1);

        if (lineNo == 0 && line == u"---") {
            inFrontmatter = true;
        } else if (inFrontmatter && (line == u"---" || line == u"...")) {
            inFrontmatter = false;
            for (const Frontmatter::Property &p : Frontmatter::parse(frontmatter)) {
                if (p.key.isEmpty())
                    continue;
                n.properties << p.key;
                if (p.key == u"tags")
                    for (const QString &t : p.items)
                        n.tags << (t.startsWith(u'#') ? t.mid(1) : t);
                else if (p.key == u"aliases")
                    n.aliases << p.items;
            }
        } else if (inFrontmatter) {
            frontmatter << line.toString();
        } else {
            QStringView body = line;
            int indent = 0;
            while (indent < body.size() && (body[indent] == u' ' || body[indent] == u'\t'))
                ++indent;
            body = body.mid(indent);
            bool isFence = false;
            if (!inFrontmatter && (body.startsWith(u"```") || body.startsWith(u"~~~"))) {
                const QChar c = body[0];
                int run = 0;
                while (run < body.size() && body[run] == c)
                    ++run;
                if (!inFence) {
                    inFence = true;
                    fenceChar = c;
                    fenceLen = run;
                    isFence = true;
                } else if (c == fenceChar && run >= fenceLen && body.mid(run).trimmed().isEmpty()) {
                    inFence = false;
                    isFence = true;
                }
            }

            if (!inFence && !isFence) {
                if (!inFrontmatter && indent == 0 && line.startsWith(u'#')) {
                    int level = 0;
                    while (level < line.size() && line[level] == u'#')
                        ++level;
                    if (level <= 6 && (level == line.size() || line[level] == u' ' || line[level] == u'\t')) {
                        QString t = line.mid(level).trimmed().toString();
                        t.remove(trailingHashes);
                        n.headings.append({level, t, lineNo});
                    }
                }

                if (const auto m = blockId.matchView(line); m.hasMatch())
                    n.blocks.insert(m.captured(1), lineNo);
                static const QRegularExpression notTags(u"(`+).*?\\1|\\[\\[[^\\]]*\\]\\]|\\]\\([^)]*\\)"_s);
                const QString plain = line.toString().replace(notTags, u" "_s);
                for (auto it = tag.globalMatch(plain); it.hasNext();)
                    n.tags << it.next().captured(1);
                qsizetype i = 0;
                while (i < line.size()) {
                    const QChar c = line[i];
                    if (c == u'`') {
                        qsizetype run = 0;
                        while (i + run < line.size() && line[i + run] == u'`')
                            ++run;
                        const QString ticks(run, u'`');
                        const qsizetype close = line.indexOf(ticks, i + run);
                        i = close < 0 ? i + run : close + run;
                        continue;
                    }
                    if (c == u'[' && i + 1 < line.size() && line[i + 1] == u'[') {
                        const qsizetype end = line.indexOf(u"]]", i + 2);
                        if (end < 0)
                            break;
                        const QStringView inner = line.mid(i + 2, end - i - 2);
                        if (!inner.trimmed().isEmpty() && !inner.contains(u'[')) {
                            Link l;
                            l.embed = i > 0 && line[i - 1] == u'!';
                            const qsizetype pipe = inner.indexOf(u'|');
                            const QStringView tgt = pipe < 0 ? inner : inner.left(pipe);
                            if (pipe >= 0)
                                l.alias = inner.mid(pipe + 1).trimmed().toString();
                            const qsizetype hash = tgt.indexOf(u'#');
                            l.target = (hash < 0 ? tgt : tgt.left(hash)).trimmed().toString();
                            if (hash >= 0)
                                l.heading = tgt.mid(hash + 1).trimmed().toString();
                            l.line = lineNo;
                            l.start = int(lineStart + i - (l.embed ? 1 : 0));
                            l.length = int(end + 2 - i + (l.embed ? 1 : 0));
                            n.links.append(l);
                        }
                        i = end + 2;
                        continue;
                    }
                    ++i;
                }
            }
        }
        ++lineNo;
        lineStart = nl + 1;
    }
    n.tags.removeDuplicates();
    return n;
}

namespace {
const QString &forbiddenChars()
{
    static const QString chars = u"\\/:*?\"<>|#^[]"_s;
    return chars;
}
}

QString Vault::nameProblem(const QString &name)
{
    if (name.trimmed().isEmpty())
        return QCoreApplication::translate("Vault", "The name is empty.");
    for (const QChar c : name)
        if (forbiddenChars().contains(c))
            return QCoreApplication::translate("Vault", "Names cannot contain any of these characters:\n%1").arg(forbiddenChars());
    if (name.startsWith(u'.'))
        return QCoreApplication::translate("Vault", "Names cannot start with a dot; files like that are hidden.");
    if (name.endsWith(u'.') || name.endsWith(u' '))
        return QCoreApplication::translate("Vault", "Names cannot end with a dot or a space.");
    if (name.toUtf8().size() > 200)
        return QCoreApplication::translate("Vault", "The name is too long; keep it under 200 characters.");
    return {};
}

QString Vault::safeName(const QString &name)
{
    QString out;
    for (const QChar c : name)
        out += forbiddenChars().contains(c) ? QChar(u'-') : c;
    out = out.trimmed();
    while (out.startsWith(u'.'))
        out.remove(0, 1);
    while (out.endsWith(u'.'))
        out.chop(1);
    return out.isEmpty() ? u"Untitled"_s : out;
}

QString Vault::resolve(const QString &target, const QString &from) const
{
    QString t = target.trimmed();
    t.replace(u'\\', u'/');
    if (t.startsWith(u'/'))
        t.remove(0, 1);
    if (t.isEmpty())
        return from;

    const QString lower = t.toLower();
    for (const QString &cand : {lower, lower + u".md"_s}) {
        const auto it = m_lowerPath.constFind(cand);
        if (it != m_lowerPath.constEnd())
            return *it;
    }

    const QString name = lower.section(u'/', -1);
    QStringList cands = m_byFileName.value(name) + m_byFileName.value(name + u".md"_s);
    if (lower.contains(u'/')) {
        cands.removeIf([&](const QString &c) {
            const QString l = c.toLower();
            return !l.endsWith(u'/' + lower) && !l.endsWith(u'/' + lower + u".md"_s);
        });
    }
    if (cands.isEmpty())
        return {};
    if (cands.size() == 1)
        return cands.first();

    const QString fromDir = dirOf(from);
    for (const QString &c : std::as_const(cands))
        if (dirOf(c) == fromDir)
            return c;
    std::sort(cands.begin(), cands.end(), [](const QString &a, const QString &b) {
        const auto da = a.count(u'/'), db = b.count(u'/');
        return da != db ? da < db : lessPath(a, b);
    });
    return cands.first();
}

QString Vault::linkText(const QString &path) const
{
    return linkTextIn(path, false);
}

QString Vault::linkTextIn(const QString &path, bool keepFullPath) const
{
    const QString fileName = path.section(u'/', -1);
    if (!keepFullPath && m_byFileName.value(fileName.toLower()).size() <= 1)
        return stripMd(fileName);
    return stripMd(path);
}

QList<QPair<QString, Link>> Vault::backlinks(const QString &path) const
{
    QList<QPair<QString, Link>> out;
    for (const QString &f : m_files) {
        const Note *n = note(f);
        if (!n || f == path)
            continue;
        for (const Link &l : n->links)
            if (resolve(l.target, f) == path)
                out.append({f, l});
    }
    return out;
}

QList<SearchHit> Vault::search(const QString &query, int limit, bool matchCase) const
{
    QList<SearchHit> hits;
    const SearchQuery q(query, matchCase);
    if (q.isEmpty())
        return hits;
    for (const QString &f : m_files) {
        const Note *n = note(f);
        if (!n || !q.matches(*n))
            continue;
        const QList<int> lines = q.hitLines(*n);
        if (lines.isEmpty())
            hits.append({f, -1, {}});
        const QStringList text = lines.isEmpty() ? QStringList() : n->text.split(u'\n');
        for (const int line : lines) {
            hits.append({f, line, text[line].trimmed()});
            if (hits.size() >= limit)
                return hits;
        }
    }
    return hits;
}

QString Vault::uniquePath(const QString &wanted) const
{
    if (!exists(wanted))
        return wanted;
    const QFileInfo fi(wanted);
    const QString dir = dirOf(wanted);
    const QString suffix = fi.suffix().isEmpty() ? QString() : u'.' + fi.suffix();
    const QString stem = fi.suffix().isEmpty() ? fi.fileName() : fi.completeBaseName();
    for (int i = 1;; ++i) {
        const QString name = u"%1 %2%3"_s.arg(stem).arg(i).arg(suffix);
        const QString p = dir.isEmpty() ? name : dir + u'/' + name;
        if (!exists(p))
            return p;
    }
}

QString Vault::createNote(const QString &path, const QString &text)
{
    if (exists(path))
        return {};
    QDir().mkpath(absolutePath(dirOf(path)));
    QSaveFile f(absolutePath(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
        return {};
    rescan();
    return path;
}

bool Vault::createFolder(const QString &path)
{
    const bool ok = QDir().mkpath(absolutePath(path));
    rescan();
    return ok;
}

bool Vault::writeNote(const QString &path, const QString &text)
{
    QDir().mkpath(absolutePath(dirOf(path)));
    QSaveFile f(absolutePath(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
        return false;
    m_notes.insert(path, parse(path, text));
    m_stamps.insert(path, fileStamp(path));
    if (!m_lowerPath.contains(path.toLower())) {
        m_files.append(path);
        std::sort(m_files.begin(), m_files.end(), lessPath);
        rebuildNameIndex();
    }
    Q_EMIT changed();
    return true;
}

QString Vault::rename(const QString &from, const QString &to)
{
    if (from == to)
        return {};
    if (to.isEmpty() || to.startsWith(u'/') || to.split(u'/').contains(u".."_s))
        return tr("Invalid name.");
    if (exists(to) && from.compare(to, Qt::CaseInsensitive) != 0)
        return tr("“%1” already exists.").arg(to);

    QHash<QString, QString> moved;
    if (isDir(from)) {
        if (to.startsWith(from + u'/'))
            return tr("A folder cannot be moved into itself.");
        for (const QString &f : std::as_const(m_files))
            if (f.startsWith(from + u'/'))
                moved.insert(f, to + f.mid(from.size()));
    } else {
        moved.insert(from, to);
    }

    // Collect link rewrites against the index as it is *before* the move.
    QHash<QString, QList<QPair<Link, QString>>> rewrites;
    QHash<QString, QString> originalText;
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
        for (const Link &l : it->links) {
            const QString r = resolve(l.target, it.key());
            if (moved.contains(r)) {
                rewrites[it.key()].append({l, moved.value(r)});
                originalText.insert(it.key(), it->text);
            }
        }
    }

    QDir().mkpath(absolutePath(dirOf(to)));
    if (!QDir().rename(absolutePath(from), absolutePath(to)))
        return tr("Could not rename “%1”.").arg(from);
    rescan();

    for (auto it = rewrites.cbegin(); it != rewrites.cend(); ++it) {
        const QString notePath = moved.value(it.key(), it.key());
        QString text = originalText.value(it.key());
        auto edits = it.value();
        std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return a.first.start > b.first.start; });
        for (const auto &[link, newPath] : std::as_const(edits)) {
            if (resolve(link.target, notePath) == newPath)
                continue; // still points to the right file, keep the user's text
            QString token = link.embed ? u"![["_s : u"[["_s;
            token += linkTextIn(newPath, link.target.contains(u'/'));
            if (!link.heading.isEmpty())
                token += u'#' + link.heading;
            if (!link.alias.isEmpty())
                token += u'|' + link.alias;
            token += u"]]"_s;
            text.replace(link.start, link.length, token);
        }
        writeNote(notePath, text);
    }

    for (auto it = moved.cbegin(); it != moved.cend(); ++it)
        Q_EMIT renamed(it.key(), it.value());
    Q_EMIT changed();
    return {};
}

bool Vault::remove(const QString &path)
{
    const bool ok = QFile::moveToTrash(absolutePath(path));
    rescan();
    return ok;
}
