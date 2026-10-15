#include "Frontmatter.h"
#include "Markdown.h"
#include "SearchQuery.h"
#include "Settings.h"
#include "TextMerge.h"
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

    void searchOperators()
    {
        const Note a = Vault::parse(u"work/plan.md"_s, u"---\ntags: [project/alpha]\n---\n# Goals\nShip the Editor\n- [ ] write docs\n- [x] fix bug #urgent\n# Notes\nrandom words ^ref1\n"_s);
        const Note b = Vault::parse(u"home/list.md"_s, u"Buy milk\n- [ ] call mom\n"_s);
        auto hits = [&](const QString &q, bool matchCase = false) {
            const SearchQuery query(q, matchCase);
            QStringList out;
            for (const Note *n : {&a, &b})
                if (query.matches(*n))
                    out << n->path;
            return out.join(u',');
        };
        QCOMPARE(a.tags, QStringList({u"project/alpha"_s, u"urgent"_s}));
        QCOMPARE(a.blocks.value(u"ref1"_s), 8);
        QCOMPARE(hits(u"editor"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"editor"_s, true), QString());
        QCOMPARE(hits(u"tag:#project"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"tag:urgent"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"tag:proj"_s), QString());
        QCOMPARE(hits(u"path:home"_s), u"home/list.md"_s);
        QCOMPARE(hits(u"file:plan"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"task-todo:call"_s), u"home/list.md"_s);
        QCOMPARE(hits(u"task-done:(fix bug)"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"task:docs"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"line:(ship editor)"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"line:(ship docs)"_s), QString());
        QCOMPARE(hits(u"section:(goals docs)"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"section:(goals random)"_s), QString());
        QCOMPARE(hits(u"milk OR docs"_s), u"work/plan.md,home/list.md"_s);
        QCOMPARE(hits(u"-milk"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"call -milk"_s), QString());
        QCOMPARE(hits(u"/b[au]y/"_s), u"home/list.md"_s);
        QCOMPARE(hits(u"\"buy milk\""_s), u"home/list.md"_s);
        QCOMPARE(SearchQuery(u"task-todo:"_s).hitLines(a), QList<int>{5});
        QCOMPARE(hits(u"milk OR"_s), u"home/list.md"_s);
        QCOMPARE(hits(u"OR milk"_s), u"home/list.md"_s);
        QVERIFY(SearchQuery(u"()"_s).isEmpty());
        QCOMPARE(hits(u"path:(work plan)"_s), u"work/plan.md"_s);
        QCOMPARE(hits(u"path:(work milk)"_s), QString());
        QCOMPARE(hits(u"tag:(nothing OR urgent)"_s), u"work/plan.md"_s);
        QCOMPARE(SearchQuery(u"tag:urgent"_s).hitLines(a), QList<int>{6});
        RenderOptions opt;
        QVERIFY(renderMarkdown(u"_penting_ ^abc"_s, opt).html.contains(u"<i>penting</i>"_s));
        QVERIFY(!renderMarkdown(u"_penting_ ^abc"_s, opt).html.contains(u"abc"_s));
    }

    void momentFormats()
    {
        const QDateTime t(QDate(2026, 3, 7), QTime(14, 5, 9));
        QCOMPARE(Settings::formatMoment(t, u"YYYY-MM-DD"_s), u"2026-03-07"_s);
        QCOMPARE(Settings::formatMoment(t, u"YYYY/MM/[Week] ww/D.M.YY"_s), u"2026/03/Week 10/7.3.26"_s);
        QCOMPARE(Settings::formatMoment(t, u"HH:mm:ss h:mm A Do"_s), u"14:05:09 2:05 PM 7th"_s);
        Settings::Templates formats{{}, u"DD/MM"_s, u"HH.mm"_s};
        QCOMPARE(Settings::applyTemplate(u"# {{title}}\n{{date}} {{time}} {{date:YYYY}} {{ date : [Q]Q }}"_s, u"Plan"_s, formats, t),
                 u"# Plan\n07/03 14.05 2026 Q1"_s);
    }

    void mergeOutsideChanges()
    {
        QString out;
        const QString base = u"one\ntwo\nthree\n"_s;
        QVERIFY(TextMerge::merge(base, u"one!\ntwo\nthree\n"_s, u"one\ntwo\nthree\nfour\n"_s, &out));
        QCOMPARE(out, u"one!\ntwo\nthree\nfour\n"_s);
        QVERIFY(TextMerge::merge(base, base + u"mine"_s, base + u"theirs\n"_s, &out));
        QCOMPARE(out, base + u"theirs\nmine"_s);
        QVERIFY(TextMerge::merge(base, u"one\nTWO\nthree\n"_s, base, &out));
        QCOMPARE(out, u"one\nTWO\nthree\n"_s);
        QVERIFY(!TextMerge::merge(base, u"one\nmine\nthree\n"_s, u"one\ntheirs\nthree\n"_s, &out));
    }

    void fileNames()
    {
        QVERIFY(Vault::nameProblem(u"Plan 🎉 ü"_s).isEmpty());
        for (const QString &bad : {u""_s, u"  "_s, u"a/b"_s, u"a:b"_s, u".hidden"_s, u"end."_s, u"end "_s, QString(201, u'x')})
            QVERIFY2(!Vault::nameProblem(bad).isEmpty(), qPrintable(bad));
        QCOMPARE(Vault::safeName(u"2026:10:15"_s), u"2026-10-15"_s);
        QCOMPARE(Vault::safeName(u"..x.."_s), u"x"_s);
        QCOMPARE(Vault::safeName(u"???"_s), u"---"_s);
    }

    void frontmatterParse()
    {
        using namespace Frontmatter;
        const QStringList src = {u"title: \"A: b\""_s, u"tags: [x, \"y z\"]"_s, u"# note"_s, u"done: TRUE"_s, u"n: 3"_s,
                                 u"when: 2024-01-02"_s, u"nested:"_s, u"  a: 1"_s, u"list:"_s, u"- one"_s, u"- 'it''s'"_s,
                                 u"text: plain # comment"_s, u"block: |"_s, u"  line"_s, u"empty:"_s};
        const QList<Property> ps = parse(src);
        QStringList keys, joined;
        for (const Property &p : ps) {
            keys << p.key;
            joined += p.lines;
        }
        QCOMPARE(keys, QStringList({u"title"_s, u"tags"_s, QString(), u"done"_s, u"n"_s, u"when"_s, u"nested"_s, u"list"_s,
                                    u"text"_s, u"block"_s, u"empty"_s}));
        QCOMPARE(joined, src);
        QCOMPARE(ps[0].value, u"A: b"_s);
        QVERIFY(ps[0].type == Type::Text);
        QCOMPARE(ps[1].items, QStringList({u"x"_s, u"y z"_s}));
        QVERIFY(ps[3].type == Type::Checkbox && ps[3].value == u"true");
        QVERIFY(ps[4].type == Type::Number);
        QVERIFY(ps[5].type == Type::Date);
        QVERIFY(ps[6].type == Type::Raw);
        QVERIFY(ps[7].type == Type::List);
        QCOMPARE(ps[7].items, QStringList({u"one"_s, u"it's"_s}));
        QCOMPARE(ps[8].value, u"plain"_s);
        QVERIFY(ps[9].type == Type::Raw);
        QVERIFY(ps[10].type == Type::Text && ps[10].value.isEmpty());
        QVERIFY(parse({u"tags: a, b"_s})[0].items == QStringList({u"a"_s, u"b"_s}));
    }

    void frontmatterSerialize()
    {
        using namespace Frontmatter;
        Property p;
        p.key = u"k"_s;
        for (const QString &v : {u"123"_s, u"true"_s, u"A: b"_s, u"#x"_s, u" padded"_s, u"plain words"_s, u"say \"hi\""_s}) {
            p.value = v;
            const QList<Property> back = parse(serialize(p));
            QCOMPARE(back.size(), 1);
            QCOMPARE(back[0].value, v);
            QVERIFY(back[0].type == Type::Text);
        }
        QCOMPARE(serialize(p).value(0), u"k: say \"hi\""_s);
        p.value.clear();
        QCOMPARE(serialize(p), QStringList{u"k:"_s});
        Property list;
        list.key = u"tags"_s;
        list.type = Type::List;
        list.items = {u"a"_s, u"b c"_s, u"2024"_s};
        list.lines = {u"tags:"_s, u"- old"_s};
        QCOMPARE(serialize(list), QStringList({u"tags:"_s, u"- a"_s, u"- b c"_s, u"- \"2024\""_s}));
        QCOMPARE(parse(serialize(list))[0].items, list.items);
        Property box;
        box.key = u"done"_s;
        box.type = Type::Checkbox;
        box.value = u"true"_s;
        QCOMPARE(serialize(box), QStringList{u"done: true"_s});
    }
};

QTEST_GUILESS_MAIN(TestVault)
#include "tst_vault.moc"
