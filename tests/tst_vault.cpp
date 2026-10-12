#include "Markdown.h"
#include "Vault.h"

#include <QDir>
#include <QSignalSpy>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

class TestVault : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    void write(const QString &rel, const QString &text)
    {
        const QString abs = m_dir.filePath(rel);
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text.toUtf8());
    }
    QString read(const QString &rel)
    {
        QFile f(m_dir.filePath(rel));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

private Q_SLOTS:
    void init()
    {
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.path());
        write(u"a/plan.md"_s, u"# Plan A\nsee [[notes]] and [[b/plan|other plan]]"_s);
        write(u"b/plan.md"_s, u"# Plan B\n## Step one\nback to [[plan]]"_s);
        write(u"notes.md"_s, u"links: [[plan]] [[a/plan#Plan A]] ![[pic.png]]\n```\n[[not a link]]\n```\n`[[nope]]`"_s);
        write(u"pic.png"_s, u"x"_s);
        write(u".obsidian/app.json"_s, u"{}"_s);
    }

    void parseSkipsCode()
    {
        const Note n = Vault::parse(u"x.md"_s, read(u"notes.md"_s));
        QCOMPARE(n.links.size(), 3);
        QCOMPARE(n.links[1].target, u"a/plan"_s);
        QCOMPARE(n.links[1].heading, u"Plan A"_s);
        QVERIFY(n.links[2].embed);
    }

    void resolvePrefersSameFolderThenShortest()
    {
        Vault v(m_dir.path());
        QVERIFY(!v.files().contains(u".obsidian/app.json"_s));
        QCOMPARE(v.resolve(u"plan"_s, u"b/x.md"_s), u"b/plan.md"_s);
        QCOMPARE(v.resolve(u"plan"_s, u"notes.md"_s), u"a/plan.md"_s);
        QCOMPARE(v.resolve(u"B/Plan"_s, u"notes.md"_s), u"b/plan.md"_s);
        QCOMPARE(v.resolve(u"pic.png"_s, u"notes.md"_s), u"pic.png"_s);
        QCOMPARE(v.resolve(u"missing"_s, u"notes.md"_s), QString());
        QCOMPARE(v.linkText(u"notes.md"_s), u"notes"_s);
        QCOMPARE(v.linkText(u"a/plan.md"_s), u"a/plan"_s);
    }

    void backlinks()
    {
        Vault v(m_dir.path());
        const auto links = v.backlinks(u"b/plan.md"_s);
        QCOMPARE(links.size(), 1);
        QCOMPARE(links[0].first, u"a/plan.md"_s);
    }

    void renameUpdatesLinks()
    {
        Vault v(m_dir.path());
        QCOMPARE(v.rename(u"notes.md"_s, u"journal.md"_s), QString());
        QVERIFY(v.exists(u"journal.md"_s));
        QCOMPARE(read(u"a/plan.md"_s), u"# Plan A\nsee [[journal]] and [[b/plan|other plan]]"_s);

        // Moving b/plan out of the way leaves a unique name: path links stay paths.
        QCOMPARE(v.rename(u"b/plan.md"_s, u"c/roadmap.md"_s), QString());
        QCOMPARE(read(u"a/plan.md"_s), u"# Plan A\nsee [[journal]] and [[c/roadmap|other plan]]"_s);
        QVERIFY(read(u"journal.md"_s).startsWith(u"links: [[plan]] [[a/plan#Plan A]] ![[pic.png]]"_s));
    }

    void renameFolderAndAttachment()
    {
        Vault v(m_dir.path());
        QCOMPARE(v.rename(u"a"_s, u"archive"_s), QString());
        QVERIFY(v.exists(u"archive/plan.md"_s));
        QVERIFY(read(u"notes.md"_s).startsWith(u"links: [[plan]] [[archive/plan#Plan A]]"_s));
        QCOMPARE(v.rename(u"pic.png"_s, u"img/photo.png"_s), QString());
        QVERIFY(read(u"notes.md"_s).contains(u"![[photo.png]]"_s));
        QVERIFY(read(u"notes.md"_s).contains(u"[[not a link]]"_s));
        QVERIFY(!v.rename(u"archive"_s, u"archive/sub"_s).isEmpty());
    }

    void search()
    {
        Vault v(m_dir.path());
        const auto hits = v.search(u"step"_s);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits[0].path, u"b/plan.md"_s);
        QCOMPARE(hits[0].line, 1);
    }

    void renderObsidianSyntax()
    {
        Vault v(m_dir.path());
        RenderOptions opt;
        opt.vault = &v;
        opt.notePath = u"notes.md"_s;
        const QString md = u"# Title\n> [!danger] Careful\n> body\n\n- [ ] todo\n- [x] done\n\n[[plan]] [[missing]] ~~old~~\n\n```bash\necho hi\n```\n"_s;
        const RenderResult r = renderMarkdown(md, opt);
        QVERIFY2(r.html.contains(u"<a name=\"h-title\">"_s), qPrintable(r.html));
        QVERIFY(r.html.contains(u"Careful"_s));
        QVERIFY(!r.html.contains(u"[!danger]"_s));
        QVERIFY(r.html.contains(u"href=\"wiki:plan\""_s));
        QVERIFY(r.html.contains(u"task:"_s));
        QVERIFY(r.html.contains(u"<s>old</s>"_s));
        const QString tilde = renderMarkdown(u"edit ~/.bashrc atau ~/.zshrc, `a~b`"_s, opt).html;
        QVERIFY2(!tilde.contains(u"<s>"_s) && tilde.contains(u"~/.bashrc atau ~/.zshrc"_s) && tilde.contains(u"a~b"_s), qPrintable(tilde));
        QCOMPARE(r.codeBlocks, QStringList{u"echo hi"_s});

        const QString embed = renderMarkdown(u"text\n![[pic.png]] and `![[code]]` wow!"_s, opt).html;
        QVERIFY2(embed.contains(u"<img src="_s) && embed.contains(u"![[code]]"_s) && embed.contains(u"wow!"_s), qPrintable(embed));
        QVERIFY(!embed.contains(QChar(0x1e)));

        // Task offsets point at the mark inside the source, after frontmatter too.
        const QString withFm = u"---\ntags: x\n---\n- [ ] a"_s;
        const QString html = renderMarkdown(withFm, opt).html;
        const int at = html.indexOf(u"task:"_s);
        const int offset = html.mid(at + 5).section(u'"', 0, 0).toInt();
        QCOMPARE(withFm.toUtf8().at(offset), ' ');
        QCOMPARE(withFm.toUtf8().mid(offset - 1, 3), QByteArray("[ ]"));
    }

    void externalEditsArePickedUp()
    {
        Vault v(m_dir.path());
        QSignalSpy changedOnDisk(&v, &Vault::noteChangedOnDisk);
        write(u"notes.md"_s, u"edited elsewhere [[b/plan]]"_s);
        QVERIFY(changedOnDisk.wait(3000));
        QCOMPARE(changedOnDisk.first().first().toString(), u"notes.md"_s);
        QCOMPARE(v.note(u"notes.md"_s)->links.size(), 1);

        QSignalSpy changed(&v, &Vault::changed);
        write(u"fresh/new.md"_s, u"hi"_s);
        QTRY_VERIFY_WITH_TIMEOUT(v.note(u"fresh/new.md"_s) != nullptr, 3000);
        QVERIFY(changed.count() > 0);
    }

    void headingAnchorsIgnoreMarkup()
    {
        QCOMPARE(headingAnchor(u"**Sumber** per tabel (tahap lanjutan)"_s), headingAnchor(u"Sumber per tabel (tahap lanjutan)"_s));
    }
};

QTEST_GUILESS_MAIN(TestVault)
#include "tst_vault.moc"
