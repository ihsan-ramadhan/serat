#include "Frontmatter.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace Frontmatter {

namespace {

const QRegularExpression &keyRe()
{
    static const QRegularExpression re(
        u"^(\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^']|'')*'|[^\\s#'\"\\-][^:]*?)[ \\t]*:(?:[ \\t]+(.*))?$"_s);
    return re;
}

bool isGap(const QString &line)
{
    const QString t = line.trimmed();
    return t.isEmpty() || t.startsWith(u'#');
}

// A plain or quoted YAML scalar; false for anything structured.
bool scalar(const QString &in, QString *out, bool *quoted)
{
    QString s = in.trimmed();
    *quoted = false;
    if (s.startsWith(u'\'') || s.startsWith(u'"')) {
        const QChar q = s[0];
        QString v;
        qsizetype i = 1;
        bool closed = false;
        for (; i < s.size(); ++i) {
            const QChar ch = s[i];
            if (q == u'\'' && ch == u'\'') {
                if (i + 1 < s.size() && s[i + 1] == u'\'') {
                    v += u'\'';
                    ++i;
                    continue;
                }
                closed = true;
                break;
            }
            if (q == u'"' && ch == u'\\' && i + 1 < s.size()) {
                const QChar e = s[++i];
                v += e == u'n' ? QChar(u'\n') : e == u't' ? QChar(u'\t') : e;
                continue;
            }
            if (q == u'"' && ch == u'"') {
                closed = true;
                break;
            }
            v += ch;
        }
        const QString rest = s.mid(i + 1).trimmed();
        if (!closed || (!rest.isEmpty() && !rest.startsWith(u'#')))
            return false;
        *out = v;
        *quoted = true;
        return true;
    }
    if (const qsizetype hash = s.indexOf(u" #"_s); hash >= 0)
        s = s.left(hash).trimmed();
    if (s.startsWith(u'#'))
        s.clear();
    if (!s.isEmpty() && u"[]{}&*!|>%@`"_s.contains(s[0]))
        return false;
    if (s.contains(u": "_s) || s.endsWith(u':') || s.startsWith(u"- "_s))
        return false;
    if (s == u"~" || s.compare(u"null"_s, Qt::CaseInsensitive) == 0)
        s.clear();
    *out = s;
    return true;
}

bool flowList(const QString &in, QStringList *items)
{
    const QString s = in.trimmed();
    if (!s.startsWith(u'[') || !s.endsWith(u']'))
        return false;
    const QString inner = s.mid(1, s.size() - 2);
    QStringList parts;
    QString cur;
    QChar quote;
    for (const QChar ch : inner) {
        if (quote.isNull() && (ch == u'[' || ch == u'{'))
            return false;
        if (quote.isNull() && ch == u',') {
            parts << cur;
            cur.clear();
            continue;
        }
        if (ch == u'"' || ch == u'\'')
            quote = quote.isNull() ? ch : quote == ch ? QChar() : quote;
        cur += ch;
    }
    parts << cur;
    for (const QString &part : std::as_const(parts)) {
        QString v;
        bool quoted = false;
        if (!scalar(part, &v, &quoted))
            return false;
        if (!v.isEmpty() || quoted)
            *items << v;
    }
    return true;
}

} // namespace

bool isDate(const QString &v)
{
    static const QRegularExpression re(u"^\\d{4}-\\d{2}-\\d{2}$"_s);
    return re.match(v).hasMatch();
}

bool isDateTime(const QString &v)
{
    static const QRegularExpression re(u"^\\d{4}-\\d{2}-\\d{2}[T ]\\d{2}:\\d{2}(:\\d{2}(\\.\\d+)?)?$"_s);
    return re.match(v).hasMatch();
}

bool isNumber(const QString &v)
{
    static const QRegularExpression re(u"^[-+]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][-+]?\\d+)?$"_s);
    return re.match(v).hasMatch();
}

namespace {

bool isBool(const QString &v)
{
    return v.compare(u"true"_s, Qt::CaseInsensitive) == 0 || v.compare(u"false"_s, Qt::CaseInsensitive) == 0;
}

Type scalarType(const QString &v, bool quoted)
{
    if (quoted || v.isEmpty())
        return Type::Text;
    if (isBool(v))
        return Type::Checkbox;
    if (isNumber(v))
        return Type::Number;
    if (isDate(v))
        return Type::Date;
    if (isDateTime(v))
        return Type::DateTime;
    return Type::Text;
}

bool needsQuotes(const QString &v, bool asText)
{
    if (v.isEmpty())
        return false;
    if (v != v.trimmed() || v.contains(u'\n') || v.contains(u'\t'))
        return true;
    if (u"[]{}&*!|>'\"%@`#,?"_s.contains(v[0]) || v == u"-" || v.startsWith(u"- "_s))
        return true;
    if (v.contains(u": "_s) || v.contains(u" #"_s) || v.endsWith(u':'))
        return true;
    if (asText && (scalarType(v, false) != Type::Text || v == u"~" || v.compare(u"null"_s, Qt::CaseInsensitive) == 0))
        return true;
    return false;
}

QString yamlText(const QString &v, bool asText = true)
{
    if (!needsQuotes(v, asText))
        return v;
    QString e = v;
    e.replace(u'\\', u"\\\\"_s).replace(u'"', u"\\\""_s).replace(u'\n', u"\\n"_s).replace(u'\t', u"\\t"_s);
    return u'"' + e + u'"';
}

} // namespace

bool fixedList(const QString &key)
{
    return key == u"tags" || key == u"aliases" || key == u"cssclasses";
}

QList<Property> parse(const QStringList &lines)
{
    static const QRegularExpression item(u"^(\\s*)-(?:[ \\t]+(.*))?$"_s);
    QList<Property> out;
    auto isKey = [](const QString &line) { return keyRe().match(line).hasMatch(); };
    int i = 0;
    while (i < lines.size()) {
        const auto m = keyRe().match(lines[i]);
        if (!m.hasMatch()) {
            Property gap;
            gap.type = Type::Raw;
            do
                gap.lines << lines[i++];
            while (i < lines.size() && !isKey(lines[i]));
            out << gap;
            continue;
        }
        Property p;
        p.lines << lines[i++];
        bool quoted = false;
        if (!scalar(m.captured(1), &p.key, &quoted) || p.key.isEmpty())
            p.key = m.captured(1);
        int end = i;
        while (end < lines.size() && !isKey(lines[end]))
            ++end;
        while (end > i && isGap(lines[end - 1]))
            --end; // trailing comments and blank lines stay put as their own entry
        const QStringList cont = lines.mid(i, end - i);
        p.lines += cont;
        i = end;

        QStringList body;
        for (const QString &l : cont)
            if (!isGap(l))
                body << l;
        const QString rest = m.captured(2).trimmed();
        p.type = Type::Raw;
        if (!rest.isEmpty() && body.isEmpty()) {
            if (rest.startsWith(u'[')) {
                if (flowList(rest, &p.items))
                    p.type = Type::List;
            } else if (scalar(rest, &p.value, &quoted)) {
                p.type = scalarType(p.value, quoted);
                if (p.type == Type::Checkbox)
                    p.value = p.value.toLower();
            }
        } else if (rest.isEmpty() || rest.startsWith(u'#')) {
            p.type = body.isEmpty() ? Type::Text : Type::List;
            for (const QString &l : std::as_const(body)) {
                const auto im = item.match(l);
                QString v;
                if (!im.hasMatch() || !scalar(im.captured(2), &v, &quoted)) {
                    p.type = Type::Raw;
                    p.items.clear();
                    break;
                }
                p.items << v;
            }
        }
        if (fixedList(p.key) && p.type != Type::Raw && p.type != Type::List) {
            static const QRegularExpression sep(u"[,\\s]+"_s);
            if (!p.value.isEmpty())
                p.items = p.key == u"tags" ? p.value.split(sep, Qt::SkipEmptyParts) : QStringList{p.value};
            p.value.clear();
            p.type = Type::List;
        }
        out << p;
    }
    return out;
}

QStringList serialize(const Property &p)
{
    if (p.type == Type::Raw)
        return p.lines;
    const QString key = yamlText(p.key, false);
    switch (p.type) {
    case Type::List: {
        if (p.items.isEmpty())
            return {key + u':'};
        static const QRegularExpression item(u"^(\\s*)-"_s);
        QString indent = u"  "_s;
        for (const QString &l : p.lines.mid(1))
            if (const auto m = item.match(l); m.hasMatch()) {
                indent = m.captured(1);
                break;
            }
        QStringList out{key + u':'};
        for (const QString &v : p.items)
            out << indent + u"- "_s + yamlText(v);
        return out;
    }
    case Type::Checkbox:
        return {key + u": "_s + (p.value == u"true" ? u"true"_s : u"false"_s)};
    case Type::Text:
        return {p.value.isEmpty() ? key + u':' : key + u": "_s + yamlText(p.value)};
    default:
        return {p.value.isEmpty() ? key + u':' : key + u": "_s + p.value};
    }
}

} // namespace Frontmatter
