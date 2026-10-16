#include "Markdown.h"

#include "CodeHighlight.h"
#include "Diagrams.h"
#include "Frontmatter.h"
#include "Settings.h"
#include "Vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QUrl>

#include <md4c.h>

using namespace Qt::StringLiterals;

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
                            float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QString headingAnchor(const QString &heading)
{
    static const QRegularExpression markup(u"[*_`~=\\[\\]]"_s);
    static const QRegularExpression nonWord(u"[^\\w]+"_s, QRegularExpression::UseUnicodePropertiesOption);
    QString s = heading;
    s.remove(markup);
    s = s.toLower().replace(nonWord, u"-"_s);
    while (s.startsWith(u'-'))
        s.remove(0, 1);
    while (s.endsWith(u'-'))
        s.chop(1);
    return u"h-"_s + s;
}

QString markdownStyleSheet(const QPalette &pal, double base)
{
    const QString fixed = Settings::monoFamily();
    const QColor link = pal.color(QPalette::Link);
    return uR"(
a { color: %1; text-decoration: none; }
p { margin-top: 0px; margin-bottom: 10px; }
h1 { font-size: %3pt; font-weight: 700; margin-top: 18px; margin-bottom: 10px; }
h2 { font-size: %4pt; font-weight: 700; margin-top: 18px; margin-bottom: 8px; }
h3 { font-size: %5pt; font-weight: 700; margin-top: 14px; margin-bottom: 6px; }
h4 { font-size: %6pt; font-weight: 700; margin-top: 12px; margin-bottom: 6px; }
h5, h6 { font-size: %7pt; font-weight: 700; margin-top: 10px; margin-bottom: 4px; }
pre, code { font-family: '%2'; }
ul, ol { margin-top: 0px; margin-bottom: 8px; }
li { margin-bottom: 2px; }
th { font-weight: 700; }
)"_s.arg(link.name(), fixed)
        .arg(base * 1.75, 0, 'f', 1)
        .arg(base * 1.45, 0, 'f', 1)
        .arg(base * 1.25, 0, 'f', 1)
        .arg(base * 1.1, 0, 'f', 1)
        .arg(base, 0, 'f', 1);
}

namespace {

// md4c strikes through a single '~', Obsidian needs "~~": lone tildes become a same-width placeholder.
constexpr char LoneTilde = '\x1a';
// md4c would read "![[x]]" as an image, so the '!' becomes a marker the renderer embeds.
constexpr char EmbedMark = '\x1e';
// Bytes of %%comments%% and ^block-ids; dropped from the output so task offsets stay valid.
constexpr char Hidden = '\x1d';

// "[^id]: text" lines (and the indented lines under them) are taken out and blanked in place.
QList<QPair<QString, QString>> takeFootnotes(QByteArray &utf8)
{
    static const QRegularExpression def(u"^\\[\\^([^\\]\\s]+)\\]:[ \\t]?(.*)$"_s);
    static const QRegularExpression more(u"^(?:    |\\t)(.*)$"_s);
    QList<QPair<QString, QString>> out;
    bool fence = false;
    qsizetype start = 0;
    int open = -1; // index in `out` still taking indented lines
    while (start <= utf8.size()) {
        qsizetype end = utf8.indexOf('\n', start);
        if (end < 0)
            end = utf8.size();
        const QString line = QString::fromUtf8(utf8.mid(start, end - start));
        const QString trimmed = line.trimmed();
        bool take = false;
        if (trimmed.startsWith(u"```"_s) || trimmed.startsWith(u"~~~"_s)) {
            fence = !fence;
            open = -1;
        } else if (!fence) {
            if (const auto m = def.match(line); m.hasMatch()) {
                out.append({m.captured(1), m.captured(2)});
                open = int(out.size()) - 1;
                take = true;
            } else if (const auto c = more.match(line); open >= 0 && c.hasMatch() && !trimmed.isEmpty()) {
                out[open].second += u'\n' + c.captured(1);
                take = true;
            } else if (!trimmed.isEmpty()) {
                open = -1;
            }
        }
        if (take)
            for (qsizetype i = start; i < end; ++i)
                utf8[i] = ' ';
        start = end + 1;
    }
    return out;
}

// Marks %%comments%% and trailing ^block-ids outside code as Hidden; a line that is only hidden becomes blank.
void hideComments(QByteArray &utf8)
{
    bool fence = false, comment = false;
    qsizetype start = 0;
    while (start <= utf8.size()) {
        qsizetype end = utf8.indexOf('\n', start);
        if (end < 0)
            end = utf8.size();
        const QByteArray line = utf8.mid(start, end - start);
        const QByteArray trimmed = line.trimmed();
        if (!comment && (trimmed.startsWith("```") || trimmed.startsWith("~~~"))) {
            fence = !fence;
        } else if (!fence) {
            bool any = false, visible = false;
            for (qsizetype i = start; i < end; ++i) {
                if (!comment && utf8[i] == '`') {
                    const qsizetype close = utf8.indexOf('`', i + 1);
                    if (close > 0 && close < end) {
                        i = close;
                        visible = true;
                        continue;
                    }
                }
                if (utf8[i] == '%' && i + 1 < end && utf8[i + 1] == '%') {
                    comment = !comment;
                    utf8[i] = utf8[i + 1] = Hidden;
                    ++i;
                    any = true;
                    continue;
                }
                if (comment) {
                    utf8[i] = Hidden;
                    any = true;
                } else if (!QChar::isSpace(uchar(utf8[i]))) {
                    visible = true;
                }
            }
            static const QRegularExpression id(u"\\s\\^[A-Za-z0-9-]+\\s*$"_s);
            if (const auto m = id.match(QString::fromLatin1(line)); m.hasMatch() && !comment) {
                // From the '^' on; the space before it stays so a closing '_' still closes.
                for (qsizetype i = start + m.capturedStart() + 1; i < start + m.capturedEnd(); ++i)
                    utf8[i] = Hidden;
                any = true;
            }
            if (any && !visible)
                for (qsizetype i = start; i < end; ++i)
                    utf8[i] = ' ';
        }
        start = end + 1;
    }
}

QString fromMd(const MD_CHAR *text, MD_SIZE size, bool keepEmbedMark = false)
{
    QString s = QString::fromUtf8(text, size).replace(QChar(LoneTilde), u'~');
    if (!keepEmbedMark)
        s.replace(QChar(EmbedMark), u'!');
    return s;
}

QString attr(const MD_ATTRIBUTE &a)
{
    return fromMd(a.text, a.size);
}

} // namespace

CalloutStyle calloutStyle(const QString &type)
{
    const QString t = type.toLower();
    auto in = [&](std::initializer_list<const char16_t *> names) {
        for (const auto *n : names)
            if (t == QStringView(n))
                return true;
        return false;
    };
    if (in({u"tip", u"hint", u"important"}))
        return {u"#00bfa5"_s, u"flame"_s};
    if (in({u"abstract", u"summary", u"tldr"}))
        return {u"#00b0ff"_s, u"clipboard"_s};
    if (in({u"info"}))
        return {u"#00b8d4"_s, u"info"_s};
    if (in({u"todo"}))
        return {u"#00b8d4"_s, u"circle-check"_s};
    if (in({u"success", u"check", u"done"}))
        return {u"#00c853"_s, u"check"_s};
    if (in({u"question", u"help", u"faq"}))
        return {u"#e0ac00"_s, u"help"_s};
    if (in({u"warning", u"caution", u"attention"}))
        return {u"#ff9100"_s, u"alert"_s};
    if (in({u"failure", u"fail", u"missing"}))
        return {u"#ff5252"_s, u"close"_s};
    if (in({u"danger", u"error"}))
        return {u"#ff1744"_s, u"zap"_s};
    if (in({u"bug"}))
        return {u"#f50057"_s, u"bug"_s};
    if (in({u"example"}))
        return {u"#7c4dff"_s, u"outline"_s};
    if (in({u"quote", u"cite"}))
        return {u"#9e9e9e"_s, u"quote"_s};
    return {u"#448aff"_s, u"pencil"_s};
}

// Up to the next heading of the same or higher level, or the block holding "^id"; all of it if missing.
QString noteSection(const QString &text, const QString &heading)
{
    const QStringList lines = text.split(u'\n');
    if (heading.startsWith(u'^')) {
        const QRegularExpression id(u"\\s\\^"_s + QRegularExpression::escape(heading.mid(1)) + u"\\s*$"_s);
        static const QRegularExpression item(u"^\\s*([-*+]|\\d+[.)])\\s"_s);
        for (int i = 0; i < lines.size(); ++i) {
            if (!id.match(lines[i]).hasMatch())
                continue;
            if (item.match(lines[i]).hasMatch())
                return lines[i].trimmed();
            int first = i, last = i;
            while (first > 0 && !lines[first - 1].trimmed().isEmpty())
                --first;
            while (last + 1 < lines.size() && !lines[last + 1].trimmed().isEmpty())
                ++last;
            return lines.mid(first, last - first + 1).join(u'\n');
        }
        return text;
    }
    const QString want = headingAnchor(heading);
    static const QRegularExpression h(u"^(#{1,6})\\s+(.*)$"_s);
    int level = 0;
    QStringList out;
    for (const QString &line : lines) {
        const auto m = h.match(line);
        if (level == 0) {
            if (m.hasMatch() && headingAnchor(m.captured(2)) == want) {
                level = int(m.capturedLength(1));
                out << line;
            }
            continue;
        }
        if (m.hasMatch() && m.capturedLength(1) <= level)
            break;
        out << line;
    }
    return out.isEmpty() ? text : out.join(u'\n');
}

namespace {

class Renderer
{
public:
    Renderer(const RenderOptions &opt, QStringList *codeBlocks)
        : m_opt(opt)
        , m_code(codeBlocks)
    {
        const QPalette &p = opt.palette;
        m_text = p.color(QPalette::Text);
        m_base = p.color(QPalette::Base);
        m_codeBg = mix(m_base, m_text, opt.dark ? 0.10 : 0.06).name();
        m_border = mix(m_base, m_text, 0.22).name();
        m_muted = mix(m_text, m_base, 0.45).name();
        m_mark = mix(m_base, QColor(u"#ffd400"_s), opt.dark ? 0.35 : 0.45).name();
    }

    QString render(const QString &input)
    {
        QString text = input;
        QByteArray prefixBytes;
        static const QRegularExpression fm(u"\\A---\\r?\\n(.*?)\\r?\\n(?:---|\\.\\.\\.)[ \\t]*(?:\\r?\\n|\\z)"_s,
                                           QRegularExpression::DotMatchesEverythingOption);
        if (const auto m = fm.match(text); m.hasMatch()) {
            renderFrontmatter(m.captured(1));
            prefixBytes = text.left(m.capturedEnd()).toUtf8();
            text = text.mid(m.capturedEnd());
        }
        m_baseOffset = int(prefixBytes.size());

        QByteArray utf8 = text.toUtf8();
        hideComments(utf8);
        m_footnoteDefs = takeFootnotes(utf8);
        for (qsizetype i = 0; i < utf8.size(); ++i) {
            if (utf8[i] == '~' && (i == 0 || utf8[i - 1] != '~') && (i + 1 == utf8.size() || utf8[i + 1] != '~'))
                utf8[i] = LoneTilde;
            else if (utf8[i] == '!' && utf8.mid(i + 1, 2) == "[[")
                utf8[i] = EmbedMark;
        }
        MD_PARSER parser{};
        parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_WIKILINKS | MD_FLAG_LATEXMATHSPANS
                       | MD_FLAG_PERMISSIVEURLAUTOLINKS | MD_FLAG_PERMISSIVEWWWAUTOLINKS;
#ifdef MD_FLAG_HIGHLIGHT
        parser.flags |= MD_FLAG_HIGHLIGHT;
#endif
        parser.enter_block = [](MD_BLOCKTYPE t, void *d, void *u) { return static_cast<Renderer *>(u)->enterBlock(t, d); };
        parser.leave_block = [](MD_BLOCKTYPE t, void *d, void *u) { return static_cast<Renderer *>(u)->leaveBlock(t, d); };
        parser.enter_span = [](MD_SPANTYPE t, void *d, void *u) { return static_cast<Renderer *>(u)->enterSpan(t, d); };
        parser.leave_span = [](MD_SPANTYPE t, void *d, void *u) { return static_cast<Renderer *>(u)->leaveSpan(t, d); };
        parser.text = [](MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
            return static_cast<Renderer *>(u)->onText(t, fromMd(s, n, t == MD_TEXT_NORMAL));
        };
        md_parse(utf8.constData(), MD_SIZE(utf8.size()), &parser, this);
        m_out.replace(QChar(EmbedMark), u'!').remove(QChar(Hidden));
        addFootnotes();
        return m_out;
    }

private:
    void renderFrontmatter(const QString &yaml)
    {
        using Frontmatter::Type;
        m_out += u"<table cellspacing=\"0\" cellpadding=\"3\" style=\"margin-bottom:12px;\">"_s;
        for (const Frontmatter::Property &p : Frontmatter::parse(yaml.split(u'\n'))) {
            if (p.key.isEmpty())
                continue;
            QString value;
            if (p.type == Type::List && p.key == u"tags") {
                QStringList tags;
                for (const QString &t : p.items)
                    tags << u"<span style=\"color:%1;\">#%2</span>"_s.arg(m_opt.palette.color(QPalette::Link).name(), t.toHtmlEscaped());
                value = tags.join(u' ');
            } else if (p.type == Type::List) {
                value = p.items.join(u", "_s).toHtmlEscaped();
            } else if (p.type == Type::Checkbox) {
                value = p.value == u"true" ? u"\u2611"_s : u"\u2610"_s;
            } else if (p.type == Type::Raw) {
                QStringList body = p.lines.mid(1);
                body.prepend(p.lines.value(0).section(u':', 1));
                value = body.join(u' ').simplified().toHtmlEscaped();
            } else {
                value = p.value.toHtmlEscaped();
            }
            m_out += u"<tr><td style=\"color:%1; padding-right:16px;\">%2</td><td>%3</td></tr>"_s.arg(m_muted, p.key.toHtmlEscaped(), value);
        }
        m_out += u"</table>"_s;
    }

    static QString alignAttr(MD_ALIGN a)
    {
        switch (a) {
        case MD_ALIGN_CENTER: return u" align=\"center\""_s;
        case MD_ALIGN_RIGHT: return u" align=\"right\""_s;
        default: return u" align=\"left\""_s;
        }
    }

    int enterBlock(MD_BLOCKTYPE type, void *detail)
    {
        switch (type) {
        case MD_BLOCK_QUOTE:
            m_quoteStart.append(m_out.size());
            break;
        case MD_BLOCK_UL:
            m_out += u"<ul>"_s;
            break;
        case MD_BLOCK_OL:
            m_out += u"<ol start=\"%1\">"_s.arg(static_cast<MD_BLOCK_OL_DETAIL *>(detail)->start);
            break;
        case MD_BLOCK_LI: {
            m_out += u"<li>"_s;
            const auto *d = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
            if (d->is_task) {
                const bool checked = d->task_mark != ' ';
                const QString box = checked ? u"☑"_s : u"☐"_s;
                if (m_opt.depth == 0)
                    m_out += u"<a href=\"task:%1\" style=\"text-decoration:none;\">%2</a>&nbsp;"_s
                                 .arg(m_baseOffset + int(d->task_mark_offset))
                                 .arg(box);
                else
                    m_out += box + u"&nbsp;"_s;
            }
            break;
        }
        case MD_BLOCK_HR:
            m_out += u"<hr>"_s;
            break;
        case MD_BLOCK_H:
            m_headingLevel = int(static_cast<MD_BLOCK_H_DETAIL *>(detail)->level);
            m_out += u"<h%1>"_s.arg(m_headingLevel);
            m_headingStart = int(m_out.size());
            m_headingPlain.clear();
            break;
        case MD_BLOCK_CODE:
            m_inCode = true;
            m_codeLang = attr(static_cast<MD_BLOCK_CODE_DETAIL *>(detail)->lang);
            m_codeBuf.clear();
            break;
        case MD_BLOCK_P:
            m_paragraphStart = m_out.size();
            m_out += u"<p>"_s;
            break;
        case MD_BLOCK_TABLE:
            m_out += u"<table cellspacing=\"0\" cellpadding=\"8\" border=\"1\" width=\"100%\" "
                     "style=\"border-collapse:collapse; border-color:%1; border-style:solid; margin-bottom:12px;\">"_s
                         .arg(m_border);
            break;
        case MD_BLOCK_TR:
            m_out += u"<tr>"_s;
            break;
        case MD_BLOCK_TH:
            m_out += u"<th%1 bgcolor=\"%2\">"_s.arg(alignAttr(static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align),
                                                     m_opt.palette.color(QPalette::AlternateBase).name());
            ++m_inCell;
            break;
        case MD_BLOCK_TD:
            m_out += u"<td%1>"_s.arg(alignAttr(static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align));
            ++m_inCell;
            break;
        default:
            break;
        }
        return 0;
    }

    int leaveBlock(MD_BLOCKTYPE type, void *)
    {
        switch (type) {
        case MD_BLOCK_QUOTE:
            finishQuote();
            break;
        case MD_BLOCK_UL: m_out += u"</ul>"_s; break;
        case MD_BLOCK_OL: m_out += u"</ol>"_s; break;
        case MD_BLOCK_LI: m_out += u"</li>"_s; break;
        case MD_BLOCK_H:
            m_out.insert(m_headingStart, u"<a name=\"%1\"></a>"_s.arg(headingAnchor(m_headingPlain)));
            m_out += u"</h%1>"_s.arg(m_headingLevel);
            m_headingStart = -1;
            break;
        case MD_BLOCK_CODE:
            m_inCode = false;
            finishCode();
            break;
        case MD_BLOCK_P:
            // A paragraph that is only $$display math$$ is centred, like a display block.
            if (m_out.mid(m_paragraphStart + 3).startsWith(u"<img src=\"math:?d=1"_s) && m_out.endsWith(u"middle;\">"_s)
                && m_out.indexOf(u"<img"_s, m_paragraphStart + 4) < 0)
                m_out.replace(m_paragraphStart, 3, u"<p align=\"center\">"_s);
            m_out += u"</p>"_s;
            break;
        case MD_BLOCK_TABLE: m_out += u"</table>"_s; break;
        case MD_BLOCK_TR: m_out += u"</tr>"_s; break;
        case MD_BLOCK_TH: m_out += u"</th>"_s; --m_inCell; break;
        case MD_BLOCK_TD: m_out += u"</td>"_s; --m_inCell; break;
        default: break;
        }
        return 0;
    }

    int enterSpan(MD_SPANTYPE type, void *detail)
    {
        if (m_imgDepth || m_inEmbed) {
            if (type == MD_SPAN_IMG)
                ++m_imgDepth;
            return 0;
        }
        switch (type) {
        case MD_SPAN_EM: m_out += u"<i>"_s; break;
        case MD_SPAN_STRONG: m_out += u"<b>"_s; break;
        case MD_SPAN_DEL: m_out += u"<s>"_s; break;
        case MD_SPAN_U: m_out += u"<u>"_s; break;
        case MD_SPAN_CODE: m_out += u"<code style=\"background-color:%1;\">"_s.arg(m_codeBg); break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            m_mathBuf.clear();
            m_inMath = true;
            break;
#ifdef MD_FLAG_HIGHLIGHT
        case MD_SPAN_MARK: m_out += u"<span style=\"background-color:%1;\">"_s.arg(m_mark); break;
#endif
        case MD_SPAN_A:
            m_out += u"<a href=\"%1\">"_s.arg(attr(static_cast<MD_SPAN_A_DETAIL *>(detail)->href).toHtmlEscaped());
            break;
        case MD_SPAN_IMG:
            ++m_imgDepth;
            m_imgSrc = attr(static_cast<MD_SPAN_IMG_DETAIL *>(detail)->src);
            m_imgAlt.clear();
            break;
        case MD_SPAN_WIKILINK: {
            const QString target = attr(static_cast<MD_SPAN_WIKILINK_DETAIL *>(detail)->target);
            if (m_out.endsWith(QChar(EmbedMark))) {
                m_out.chop(1);
                m_inEmbed = true;
                m_embedTarget = target;
                m_embedLabel.clear();
                break;
            }
            const QString name = target.section(u'#', 0, 0);
            const bool resolved = name.isEmpty() || !m_opt.vault || !m_opt.vault->resolve(name, m_opt.notePath).isEmpty();
            m_out += u"<a href=\"wiki:%1\"%2>"_s.arg(QString::fromUtf8(QUrl::toPercentEncoding(target)),
                                                     resolved ? QString() : u" style=\"color:%1;\""_s.arg(m_muted));
            break;
        }
        default:
            break;
        }
        return 0;
    }

    int leaveSpan(MD_SPANTYPE type, void *)
    {
        if (m_imgDepth && (type != MD_SPAN_IMG || m_imgDepth > 1)) {
            if (type == MD_SPAN_IMG)
                --m_imgDepth;
            return 0;
        }
        if (m_inEmbed && type != MD_SPAN_WIKILINK)
            return 0;
        switch (type) {
        case MD_SPAN_EM: m_out += u"</i>"_s; break;
        case MD_SPAN_STRONG: m_out += u"</b>"_s; break;
        case MD_SPAN_DEL: m_out += u"</s>"_s; break;
        case MD_SPAN_U: m_out += u"</u>"_s; break;
        case MD_SPAN_CODE: m_out += u"</code>"_s; break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            m_inMath = false;
            // Drawn by the reader on demand, at its font size and colour.
            m_out += u"<img src=\"math:?d=%3&amp;tex=%1\" title=\"%2\" style=\"vertical-align: middle;\">"_s.arg(
                QString::fromLatin1(QUrl::toPercentEncoding(m_mathBuf)), m_mathBuf.toHtmlEscaped(),
                type == MD_SPAN_LATEXMATH_DISPLAY ? u"1"_s : u"0"_s);
            break;
#ifdef MD_FLAG_HIGHLIGHT
        case MD_SPAN_MARK: m_out += u"</span>"_s; break;
#endif
        case MD_SPAN_A: m_out += u"</a>"_s; break;
        case MD_SPAN_IMG:
            m_imgDepth = 0;
            finishImage();
            break;
        case MD_SPAN_WIKILINK:
            if (m_inEmbed) {
                m_inEmbed = false;
                finishEmbed();
            } else {
                m_out += u"</a>"_s;
            }
            break;
        default:
            break;
        }
        return 0;
    }

    int onText(MD_TEXTTYPE type, const QString &s)
    {
        if (m_inMath) {
            m_mathBuf += s;
            return 0;
        }
        if (m_inCode) {
            m_codeBuf += s;
            return 0;
        }
        if (m_imgDepth) {
            m_imgAlt += s;
            return 0;
        }
        if (m_inEmbed) {
            m_embedLabel += s;
            return 0;
        }
        if (m_headingStart >= 0 && (type == MD_TEXT_NORMAL || type == MD_TEXT_CODE))
            m_headingPlain += s;
        switch (type) {
        case MD_TEXT_NULLCHAR: m_out += QChar(0xFFFD); break;
        case MD_TEXT_BR: m_out += u"<br>"_s; break;
        // Obsidian renders single newlines as line breaks by default.
        case MD_TEXT_SOFTBR: m_out += m_inCell ? u" "_s : u"<br>"_s; break;
        case MD_TEXT_HTML:
        case MD_TEXT_ENTITY: m_out += s; break;
        default: m_out += s.toHtmlEscaped(); break;
        }
        return 0;
    }

    QString imageTag(const QString &absPath, int width) const
    {
        QImageReader reader(absPath);
        const QSize natural = reader.size();
        int w = width > 0 ? width : natural.width();
        if (w <= 0)
            w = m_opt.contentWidth;
        w = qMin(w, m_opt.contentWidth);
        return u"<img src=\"%1\" width=\"%2\">"_s.arg(QUrl::fromLocalFile(absPath).toString().toHtmlEscaped()).arg(w);
    }

    void finishImage()
    {
        const QUrl url(m_imgSrc);
        if (!url.isRelative() && !url.isLocalFile()) {
            m_out += u"<a href=\"%1\">%2</a>"_s.arg(m_imgSrc.toHtmlEscaped(),
                                                   (m_imgAlt.isEmpty() ? m_imgSrc : m_imgAlt).toHtmlEscaped());
            return;
        }
        QString abs;
        const QString path = QUrl::fromPercentEncoding(m_imgSrc.toUtf8());
        if (url.isLocalFile()) {
            abs = url.toLocalFile();
        } else if (m_opt.vault) {
            const QString r = m_opt.vault->resolve(path, m_opt.notePath);
            abs = r.isEmpty() ? QFileInfo(m_opt.vault->absolutePath(m_opt.notePath)).dir().filePath(path)
                              : m_opt.vault->absolutePath(r);
        }
        if (QFileInfo::exists(abs))
            m_out += imageTag(abs, 0);
        else
            m_out += u"<span style=\"color:%1;\">%2</span>"_s.arg(m_muted, (m_imgAlt.isEmpty() ? m_imgSrc : m_imgAlt).toHtmlEscaped());
    }

    void finishEmbed()
    {
        const QString name = m_embedTarget.section(u'#', 0, 0);
        const QString heading = m_embedTarget.section(u'#', 1);
        const QString path = m_opt.vault ? m_opt.vault->resolve(name, m_opt.notePath) : QString();
        if (path.isEmpty()) {
            m_out += u"<a href=\"wiki:%1\" style=\"color:%2;\">%3</a>"_s.arg(QString::fromUtf8(QUrl::toPercentEncoding(m_embedTarget)),
                                                                          m_muted, m_embedTarget.toHtmlEscaped());
            return;
        }
        if (Vault::isImage(path)) {
            bool ok = false;
            const int width = m_embedLabel.section(u'x', 0, 0).toInt(&ok);
            m_out += imageTag(m_opt.vault->absolutePath(path), ok && m_embedLabel != m_embedTarget ? width : 0);
            return;
        }
        const Note *note = m_opt.vault->note(path);
        if (!note || m_opt.depth >= 2) {
            m_out += u"<a href=\"wiki:%1\">%2</a>"_s.arg(QString::fromUtf8(QUrl::toPercentEncoding(m_embedTarget)),
                                                        m_embedTarget.toHtmlEscaped());
            return;
        }
        RenderOptions sub = m_opt;
        sub.notePath = path;
        sub.depth = m_opt.depth + 1;
        sub.contentWidth = m_opt.contentWidth - 40;
        Renderer r(sub, m_code);
        const QString inner = r.render(heading.isEmpty() ? note->text : noteSection(note->text, heading));
        const QString title = u"<p><a href=\"wiki:%1\"><b>%2</b></a></p>"_s.arg(
            QString::fromUtf8(QUrl::toPercentEncoding(m_embedTarget)), Vault::baseName(path).toHtmlEscaped());
        m_out += box(m_border, m_opt.palette.color(QPalette::Base).name(), title + inner);
    }

    QString box(const QString &bar, const QString &bg, const QString &content) const
    {
        return u"<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:4px; margin-bottom:12px;\">"
               "<tr><td width=\"3\" bgcolor=\"%1\"></td>"
               "<td bgcolor=\"%2\" style=\"padding-left:14px; padding-right:12px; padding-top:8px; padding-bottom:2px;\">%3</td>"
               "</tr></table>"_s.arg(bar, bg, content);
    }

    void finishQuote()
    {
        const int start = m_quoteStart.takeLast();
        QString inner = m_out.mid(start);
        m_out.truncate(start);

        static const QRegularExpression callout(u"^<p>\\[!([A-Za-z][\\w-]*)\\]([+-]?)[ ]*(.*?)(<br>|</p>)"_s);
        const auto m = callout.match(inner);
        if (!m.hasMatch()) {
            m_out += box(m_border, m_opt.palette.color(QPalette::Base).name(), inner);
            return;
        }
        const QString type = m.captured(1);
        QString title = m.captured(3).trimmed();
        if (title.isEmpty())
            title = type.left(1).toUpper() + type.mid(1).toLower();
        QString rest = inner.mid(m.capturedEnd());
        if (m.captured(4) == u"<br>")
            rest.prepend(u"<p>"_s);
        const CalloutStyle style = calloutStyle(type);
        const QString bg = mix(m_base, QColor(style.color), m_opt.dark ? 0.14 : 0.09).name();
        const QString icon = u"<img src=\"icon:%1?color=%2\" width=\"18\" height=\"18\" align=\"middle\">"_s.arg(
            style.icon, QString::fromLatin1(QUrl::toPercentEncoding(style.color)));
        QString head = u"<b>%1&nbsp;&nbsp;%2</b>"_s.arg(icon, title);
        if (!m.captured(2).isEmpty()) {
            const int index = m_callouts++;
            const bool open = (m.captured(2) == u"+") != (m_opt.depth == 0 && m_opt.toggledCallouts.contains(index));
            if (!open)
                rest.clear();
            const QString chevron = open ? u"\u25BE"_s : u"\u25B8"_s;
            head += u"&nbsp;&nbsp;"_s + chevron;
            if (m_opt.depth == 0)
                head = u"<a href=\"callout:%1\" style=\"color:%2; text-decoration:none;\">%3</a>"_s.arg(index).arg(style.color, head);
        }
        m_out += box(style.color, bg, u"<p style=\"color:%1;\">%2</p>"_s.arg(style.color, head) + rest);
    }

    // "[^id]" and inline "^[text]" become numbered references, listed under a rule at the end.
    void addFootnotes()
    {
        static const QRegularExpression code(u"<pre[\\s\\S]*?</pre>|<code[^>]*>[\\s\\S]*?</code>"_s);
        static const QRegularExpression ref(u"\\[\\^([^\\]\\s]+)\\]|\\^\\[([^\\]]+)\\]"_s);
        QHash<QString, QString> defined;
        for (const auto &[id, text] : std::as_const(m_footnoteDefs))
            defined.insert(id, text);
        QList<QPair<QString, QString>> used; // anchor id, text
        QHash<QString, int> numbers;
        auto replaceIn = [&](const QString &html) {
            QString out;
            qsizetype last = 0;
            for (auto it = ref.globalMatch(html); it.hasNext();) {
                const auto m = it.next();
                QString id, text;
                if (m.capturedLength(1)) {
                    if (!defined.contains(m.captured(1)))
                        continue;
                    id = m.captured(1);
                    text = defined.value(id);
                } else {
                    id = u"inline-%1"_s.arg(used.size() + 1);
                    text = m.captured(2);
                }
                if (!numbers.contains(id)) {
                    numbers.insert(id, int(used.size()) + 1);
                    used.append({id, text});
                }
                const QString anchor = QString::fromLatin1(QUrl::toPercentEncoding(id));
                out += html.mid(last, m.capturedStart() - last);
                out += u"<sup><a href=\"#fn-%1\" name=\"fnref-%1\" style=\"text-decoration:none;\">%2</a></sup>"_s.arg(anchor).arg(numbers.value(id));
                last = m.capturedEnd();
            }
            return out + html.mid(last);
        };
        QString out;
        qsizetype last = 0;
        for (auto it = code.globalMatch(m_out); it.hasNext();) {
            const auto m = it.next();
            out += replaceIn(m_out.mid(last, m.capturedStart() - last)) + m.captured();
            last = m.capturedEnd();
        }
        out += replaceIn(m_out.mid(last));
        m_out = out;
        if (used.isEmpty())
            return;
        m_out += u"<hr><ol style=\"color:%1;\">"_s.arg(m_muted);
        for (const auto &[id, text] : std::as_const(used)) {
            RenderOptions sub = m_opt;
            sub.depth = qMax(1, m_opt.depth);
            Renderer r(sub, m_code);
            QString body = r.render(text);
            static const QRegularExpression para(u"^<p>|</p>$"_s);
            body = body.trimmed().remove(para);
            const QString anchor = QString::fromLatin1(QUrl::toPercentEncoding(id));
            m_out += u"<li><a name=\"fn-%1\"></a>%2 <a href=\"#fnref-%1\" style=\"text-decoration:none;\">\u21A9</a></li>"_s.arg(anchor, body);
        }
        m_out += u"</ol>"_s;
    }

    void finishCode()
    {
        QString code = m_codeBuf;
        if (code.endsWith(u'\n'))
            code.chop(1);
        if (m_codeLang.compare(u"mermaid"_s, Qt::CaseInsensitive) == 0) {
            Diagrams &diagrams = Diagrams::instance();
            if (!diagrams.image(code, m_opt.dark).isNull()) {
                m_out += u"<p align=\"center\"><img src=\"diagram:?dark=%1&amp;src=%2\"></p>"_s.arg(
                    m_opt.dark ? 1 : 0).arg(QString::fromLatin1(QUrl::toPercentEncoding(code)));
                return;
            }
            const QString why = Diagrams::renderer().isEmpty() ? QCoreApplication::translate("Markdown", "Install mermaid-cli (the mmdc command) to see this diagram drawn.")
                              : diagrams.pending(code, m_opt.dark) ? QCoreApplication::translate("Markdown", "Drawing the diagram…")
                                                                    : diagrams.error(code, m_opt.dark);
            if (!why.isEmpty())
                m_out += u"<p style=\"color:%1;\"><small>%2</small></p>"_s.arg(m_muted, why.toHtmlEscaped());
        }
        const qsizetype id = m_code->size();
        m_code->append(code);
        const QString label = m_codeLang.isEmpty() ? QString() : m_codeLang.toHtmlEscaped() + u"&nbsp;&nbsp;·&nbsp;&nbsp;"_s;
        m_out += u"<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"%1\" style=\"margin-top:2px; margin-bottom:12px;\">"
                 "<tr><td><p align=\"right\" style=\"margin:0px;\"><a href=\"copy:%2\" style=\"color:%3; text-decoration:none;\">"
                 "<small>%4%6</small></a></p><pre style=\"margin:0px; white-space:pre-wrap;\">%5</pre></td></tr></table>"_s
                     .arg(m_codeBg)
                     .arg(id)
                     .arg(m_muted, label, highlightCodeHtml(code, m_codeLang, m_opt.dark), QCoreApplication::translate("Markdown", "Copy"));
    }

    const RenderOptions &m_opt;
    QStringList *m_code;
    QString m_out;
    int m_baseOffset = 0;

    QColor m_text, m_base;
    QString m_codeBg, m_border, m_muted, m_mark;

    int m_callouts = 0; // foldable ones seen so far
    QList<QPair<QString, QString>> m_footnoteDefs;
    bool m_inMath = false;
    QString m_mathBuf;
    qsizetype m_paragraphStart = 0;
    bool m_inCode = false;
    QString m_codeLang, m_codeBuf;
    int m_imgDepth = 0;
    QString m_imgSrc, m_imgAlt;
    bool m_inEmbed = false;
    QString m_embedTarget, m_embedLabel;
    QList<int> m_quoteStart;
    int m_headingStart = -1;
    int m_headingLevel = 1;
    QString m_headingPlain;
    int m_inCell = 0;
};

} // namespace

RenderResult renderMarkdown(const QString &text, const RenderOptions &opt)
{
    RenderResult result;
    Renderer r(opt, &result.codeBlocks);
    result.html = r.render(text);
    return result;
}

QString renderTableCell(const QString &cell, const RenderOptions &opt)
{
    // Parse it as a one-cell table so it follows GFM cell rules (inline only).
    const QString html = renderMarkdown(u"| "_s + QString(cell).replace(u'|', u"\\|"_s) + u" |\n| --- |\n"_s, opt).html;
    const qsizetype open = html.indexOf(u"<th"_s);
    const qsizetype start = html.indexOf(u'>', open) + 1;
    const qsizetype end = html.lastIndexOf(u"</th>"_s);
    return open < 0 || end < start ? cell.toHtmlEscaped() : html.mid(start, end - start);
}
