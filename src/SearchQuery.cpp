#include "SearchQuery.h"

#include "Vault.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

struct SearchQuery::Node {
    enum Kind { All, Any, Not, Text, Op } kind = All;
    QList<std::shared_ptr<Node>> kids; // All, Any, Not
    QString op;                        // Op: file, path, tag, content, line, section, task, task-todo, task-done
    QString text;
    QRegularExpression re;
    std::shared_ptr<Node> sub;         // Op: a word, a phrase or a ( group )
    mutable QRegularExpression tagInLine; // built on first use by tag: line hits
};

using NodeP = std::shared_ptr<SearchQuery::Node>;

namespace {

// "()" or a dangling OR leaves an empty group behind; it is dropped rather than matching everything.
bool empty(const NodeP &n)
{
    return !n || (n->kind == SearchQuery::Node::All && n->kids.isEmpty());
}

class Parser
{
public:
    Parser(const QString &text, bool matchCase)
        : s(text)
        , m_case(matchCase)
    {
    }

    NodeP parseAny()
    {
        NodeP left = parseAll();
        for (;;) {
            skipSpace();
            if (!peekOr())
                return left;
            i += 2;
            NodeP right = parseAll();
            if (empty(left)) {
                left = right;
            } else if (!empty(right)) {
                auto any = std::make_shared<SearchQuery::Node>();
                any->kind = SearchQuery::Node::Any;
                any->kids = {left, right};
                left = any;
            }
        }
    }

private:
    void skipSpace()
    {
        while (i < s.size() && s[i].isSpace())
            ++i;
    }
    bool peekOr() const { return s.mid(i, 2) == u"OR" && (i + 2 == s.size() || s[i + 2].isSpace()); }

    NodeP parseAll()
    {
        auto all = std::make_shared<SearchQuery::Node>();
        for (;;) {
            skipSpace();
            if (i >= s.size() || s[i] == u')' || peekOr())
                break;
            if (NodeP n = parseUnary(); !empty(n))
                all->kids << n;
        }
        return all->kids.size() == 1 ? all->kids.first() : all;
    }

    NodeP parseUnary()
    {
        skipSpace();
        if (s[i] == u'-' && i + 1 < s.size() && !s[i + 1].isSpace()) {
            ++i;
            NodeP inner = parseUnary();
            if (empty(inner))
                return inner;
            auto n = std::make_shared<SearchQuery::Node>();
            n->kind = SearchQuery::Node::Not;
            n->kids = {inner};
            return n;
        }
        if (s[i] == u'(') {
            ++i;
            NodeP n = parseAny();
            skipSpace();
            if (i < s.size() && s[i] == u')')
                ++i;
            return n;
        }
        static const QRegularExpression op(u"\\G(file|path|tag|content|line|section|task-todo|task-done|task):"_s);
        if (const auto m = op.match(s, i); m.hasMatch()) {
            i = m.capturedEnd();
            auto n = std::make_shared<SearchQuery::Node>();
            n->kind = SearchQuery::Node::Op;
            n->op = m.captured(1);
            if (i < s.size() && s[i] == u'(') {
                ++i;
                n->sub = parseAny();
                if (empty(n->sub))
                    n->sub.reset();
                skipSpace();
                if (i < s.size() && s[i] == u')')
                    ++i;
            } else if (i < s.size() && !s[i].isSpace()) {
                n->sub = atom();
            }
            if (n->sub && n->sub->kind == SearchQuery::Node::Text) {
                n->text = n->sub->text;
                n->re = n->sub->re;
            }
            return n;
        }
        return atom();
    }

    NodeP atom()
    {
        auto n = std::make_shared<SearchQuery::Node>();
        n->kind = SearchQuery::Node::Text;
        const auto options = m_case ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption;
        if (s[i] == u'"' || s[i] == u'/') {
            const QChar end = s[i];
            qsizetype j = i + 1;
            while (j < s.size() && s[j] != end)
                j += s[j] == u'\\' && end == u'/' ? 2 : 1;
            const QString inner = s.mid(i + 1, j - i - 1);
            i = qMin(j + 1, s.size());
            n->text = inner;
            n->re = QRegularExpression(end == u'/' ? inner : QRegularExpression::escape(inner), options);
            return n;
        }
        const qsizetype start = i;
        while (i < s.size() && !s[i].isSpace() && s[i] != u')')
            ++i;
        n->text = s.mid(start, i - start);
        n->re = QRegularExpression(QRegularExpression::escape(n->text), options);
        return n;
    }

    QString s;
    qsizetype i = 0;
    bool m_case;
};

const QRegularExpression &taskRe()
{
    static const QRegularExpression re(u"^\\s*([-*+]|\\d+[.)])\\s+\\[(.)\\]"_s);
    return re;
}

bool isTask(const QString &line, const QString &op)
{
    const auto m = taskRe().match(line);
    if (!m.hasMatch())
        return false;
    const bool done = m.captured(2) != u" ";
    return op == u"task" || (op == u"task-done") == done;
}

bool found(const SearchQuery::Node &n, const QString &text)
{
    return !n.text.isEmpty() && n.re.isValid() && n.re.match(text).hasMatch();
}

struct Context {
    const Note &note;
    const QStringList &lines;
};

bool eval(const SearchQuery::Node &n, const Context &ctx, const QString &text, bool fileLevel);

QString tagOf(const QString &text)
{
    return (text.startsWith(u'#') ? text.mid(1) : text).toLower();
}

// file:, path:, content: and tag: on one word or phrase.
bool fieldMatch(const QString &op, const SearchQuery::Node &leaf, const Context &ctx, const QString &text)
{
    if (op == u"file")
        return found(leaf, ctx.note.path.section(u'/', -1));
    if (op == u"path")
        return found(leaf, ctx.note.path);
    if (op == u"content")
        return found(leaf, text);
    const QString want = tagOf(leaf.text);
    return !want.isEmpty() && std::any_of(ctx.note.tags.cbegin(), ctx.note.tags.cend(), [&](const QString &t) {
        const QString tag = t.toLower();
        return tag == want || tag.startsWith(want + u'/');
    });
}

// A ( group ) given to file:, path:, content: or tag: applies that field to each of its words.
bool fieldEval(const QString &op, const SearchQuery::Node &n, const Context &ctx, const QString &text)
{
    using K = SearchQuery::Node;
    switch (n.kind) {
    case K::All:
        return std::all_of(n.kids.cbegin(), n.kids.cend(), [&](const NodeP &k) { return fieldEval(op, *k, ctx, text); });
    case K::Any:
        return std::any_of(n.kids.cbegin(), n.kids.cend(), [&](const NodeP &k) { return fieldEval(op, *k, ctx, text); });
    case K::Not:
        return !fieldEval(op, *n.kids.first(), ctx, text);
    case K::Text:
        return fieldMatch(op, n, ctx, text);
    case K::Op:
        return eval(n, ctx, text, false);
    }
    return false;
}

bool isField(const QString &op)
{
    return op == u"file" || op == u"path" || op == u"content" || op == u"tag";
}

// `text` is the whole note, a section or a line; the file-level operators always look at the note.
bool eval(const SearchQuery::Node &n, const Context &ctx, const QString &text, bool fileLevel)
{
    using K = SearchQuery::Node;
    switch (n.kind) {
    case K::All:
        return std::all_of(n.kids.cbegin(), n.kids.cend(), [&](const NodeP &k) { return eval(*k, ctx, text, fileLevel); });
    case K::Any:
        return std::any_of(n.kids.cbegin(), n.kids.cend(), [&](const NodeP &k) { return eval(*k, ctx, text, fileLevel); });
    case K::Not:
        return !eval(*n.kids.first(), ctx, text, fileLevel);
    case K::Text:
        return found(n, text) || (fileLevel && found(n, ctx.note.path));
    case K::Op:
        break;
    }
    if (isField(n.op))
        return n.sub && n.sub->kind != K::Text ? fieldEval(n.op, *n.sub, ctx, text) : fieldMatch(n.op, n, ctx, text);
    if (n.op == u"line")
        return n.sub && std::any_of(ctx.lines.cbegin(), ctx.lines.cend(), [&](const QString &l) { return eval(*n.sub, ctx, l, false); });
    if (n.op == u"section") {
        static const QRegularExpression heading(u"^#{1,6}\\s"_s);
        QString section;
        for (const QString &l : ctx.lines) {
            if (heading.match(l).hasMatch()) {
                if (n.sub && eval(*n.sub, ctx, section, false))
                    return true;
                section.clear();
            }
            section += l + u'\n';
        }
        return n.sub && eval(*n.sub, ctx, section, false);
    }
    return std::any_of(ctx.lines.cbegin(), ctx.lines.cend(),
                       [&](const QString &l) { return isTask(l, n.op) && (!n.sub || eval(*n.sub, ctx, l, false)); });
}

bool tagInLine(const SearchQuery::Node &leaf, const QString &line)
{
    const QString tag = tagOf(leaf.text);
    if (tag.isEmpty())
        return false;
    if (!leaf.tagInLine.isValid() || leaf.tagInLine.pattern().isEmpty())
        leaf.tagInLine = QRegularExpression(u"(^|\\s)#"_s + QRegularExpression::escape(tag) + u"(/|\\b)"_s,
                                            QRegularExpression::CaseInsensitiveOption);
    return leaf.tagInLine.match(line).hasMatch();
}

bool lineHit(const SearchQuery::Node &n, const Context &ctx, const QString &line, const QString &field = {})
{
    using K = SearchQuery::Node;
    switch (n.kind) {
    case K::All:
    case K::Any:
        return std::any_of(n.kids.cbegin(), n.kids.cend(), [&](const NodeP &k) { return lineHit(*k, ctx, line, field); });
    case K::Not:
        return false;
    case K::Text:
        if (field == u"tag")
            return tagInLine(n, line);
        return (field.isEmpty() || field == u"content") && found(n, line);
    case K::Op:
        break;
    }
    if (n.op == u"content" || n.op == u"tag")
        return n.sub && n.sub->kind != K::Text ? lineHit(*n.sub, ctx, line, n.op)
                                               : (n.op == u"tag" ? tagInLine(n, line) : found(n, line));
    if (n.op == u"line")
        return n.sub && eval(*n.sub, ctx, line, false);
    if (n.op == u"section")
        return n.sub && lineHit(*n.sub, ctx, line);
    if (n.op.startsWith(u"task"))
        return isTask(line, n.op) && (!n.sub || eval(*n.sub, ctx, line, false));
    return false;
}

} // namespace

SearchQuery::SearchQuery(const QString &query, bool matchCase)
    : m_root(Parser(query.trimmed(), matchCase).parseAny())
{
}

SearchQuery::~SearchQuery() = default;

bool SearchQuery::isEmpty() const
{
    return m_root->kind == Node::All && m_root->kids.isEmpty();
}

bool SearchQuery::matches(const Note &note) const
{
    if (isEmpty())
        return false;
    const QStringList lines = note.text.split(u'\n');
    return eval(*m_root, Context{note, lines}, note.text, true);
}

QList<int> SearchQuery::hitLines(const Note &note) const
{
    QList<int> out;
    const QStringList lines = note.text.split(u'\n');
    const Context ctx{note, lines};
    for (int i = 0; i < lines.size(); ++i)
        if (lineHit(*m_root, ctx, lines[i]))
            out << i;
    return out;
}
