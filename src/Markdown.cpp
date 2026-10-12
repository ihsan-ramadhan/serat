#include "Markdown.h"

#include "CodeHighlight.h"
#include "Vault.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
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
    const QString fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
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

// md4c follows GFM and treats a single '~' as strikethrough; Obsidian only
// accepts "~~". Lone tildes are swapped for a same-width placeholder before
// parsing (keeps byte offsets intact) and swapped back here.
constexpr char LoneTilde = '\x1a';
// "![[x]]" would make md4c start an image at "![" and miss the wikilink, so
// the '!' is swapped for a marker the renderer turns into an embed.
constexpr char EmbedMark = '\x1e';

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

struct Callout {
    QString color;
    QString icon;
};

Callout calloutStyle(const QString &type)
{
    const QString t = type.toLower();
    auto in = [&](std::initializer_list<const char16_t *> names) {
        for (const auto *n : names)
            if (t == QStringView(n))
                return true;
        return false;
    };
    if (in({u"tip", u"hint", u"important"}))
        return {u"#00bfa5"_s, u"🔥"_s};
    if (in({u"abstract", u"summary", u"tldr"}))
        return {u"#00b0ff"_s, u"📋"_s};
    if (in({u"info", u"todo"}))
        return {u"#00b8d4"_s, u"ℹ️"_s};
    if (in({u"success", u"check", u"done"}))
        return {u"#00c853"_s, u"✅"_s};
    if (in({u"question", u"help", u"faq"}))
        return {u"#e0ac00"_s, u"❓"_s};
    if (in({u"warning", u"caution", u"attention"}))
        return {u"#ff9100"_s, u"⚠️"_s};
    if (in({u"failure", u"fail", u"missing"}))
        return {u"#ff5252"_s, u"❌"_s};
    if (in({u"danger", u"error"}))
        return {u"#ff1744"_s, u"⚡"_s};
    if (in({u"bug"}))
        return {u"#f50057"_s, u"🐞"_s};
    if (in({u"example"}))
        return {u"#7c4dff"_s, u"📑"_s};
    if (in({u"quote", u"cite"}))
        return {u"#9e9e9e"_s, u"❝"_s};
    return {u"#448aff"_s, u"✏️"_s};
}

// Extracts the section under `heading` (until the next heading of the same or
// higher level). Returns the whole text if the heading is not found.
QString sectionOf(const QString &text, const QString &heading)
{
    const QStringList lines = text.split(u'\n');
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
        for (qsizetype i = 0; i < utf8.size(); ++i) {
            if (utf8[i] == '~' && (i == 0 || utf8[i - 1] != '~') && (i + 1 == utf8.size() || utf8[i + 1] != '~'))
                utf8[i] = LoneTilde;
            else if (utf8[i] == '!' && utf8.mid(i + 1, 2) == "[[")
                utf8[i] = EmbedMark;
        }
        MD_PARSER parser{};
        parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_WIKILINKS
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
        return m_out.replace(QChar(EmbedMark), u'!');
    }

private:
    void renderFrontmatter(const QString &yaml)
    {
        m_out += u"<table cellspacing=\"0\" cellpadding=\"3\" style=\"margin-bottom:12px; color:%1;\">"_s.arg(m_muted);
        for (const QString &line : yaml.split(u'\n')) {
            const qsizetype colon = line.indexOf(u':');
            if (colon <= 0 || line.startsWith(u' ') || line.startsWith(u'\t')) {
                m_out += u"<tr><td></td><td>%1</td></tr>"_s.arg(line.trimmed().toHtmlEscaped());
                continue;
            }
            m_out += u"<tr><td><b>%1</b></td><td>%2</td></tr>"_s.arg(line.left(colon).toHtmlEscaped(),
                                                                    line.mid(colon + 1).trimmed().toHtmlEscaped());
        }
        m_out += u"</table>"_s;
    }

    static QString alignAttr(MD_ALIGN a)
    {
        switch (a) {
        case MD_ALIGN_CENTER: return u" align=\"center\""_s;
        case MD_ALIGN_RIGHT: return u" align=\"right\""_s;
        default: return {};
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
            m_out += u"<p>"_s;
            break;
        case MD_BLOCK_TABLE:
            m_out += u"<table cellspacing=\"0\" cellpadding=\"6\" border=\"1\" width=\"100%\" "
                     "style=\"border-collapse:collapse; border-color:%1; border-style:solid; margin-bottom:12px;\">"_s
                         .arg(m_border);
            break;
        case MD_BLOCK_TR:
            m_out += u"<tr>"_s;
            break;
        case MD_BLOCK_TH:
            m_out += u"<th%1 bgcolor=\"%2\">"_s.arg(alignAttr(static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align), m_codeBg);
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
        case MD_BLOCK_P: m_out += u"</p>"_s; break;
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
        const QString inner = r.render(heading.isEmpty() ? note->text : sectionOf(note->text, heading));
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
        const Callout style = calloutStyle(type);
        const QString bg = mix(m_base, QColor(style.color), m_opt.dark ? 0.14 : 0.09).name();
        const QString head = u"<p style=\"color:%1;\"><b>%2&nbsp;&nbsp;%3</b></p>"_s.arg(style.color, style.icon, title);
        m_out += box(style.color, bg, head + rest);
    }

    void finishCode()
    {
        QString code = m_codeBuf;
        if (code.endsWith(u'\n'))
            code.chop(1);
        const qsizetype id = m_code->size();
        m_code->append(code);
        const QString label = m_codeLang.isEmpty() ? QString() : m_codeLang.toHtmlEscaped() + u"&nbsp;&nbsp;·&nbsp;&nbsp;"_s;
        m_out += u"<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"%1\" style=\"margin-top:2px; margin-bottom:12px;\">"
                 "<tr><td><p align=\"right\" style=\"margin:0px;\"><a href=\"copy:%2\" style=\"color:%3; text-decoration:none;\">"
                 "<small>%4Copy</small></a></p><pre style=\"margin:0px; white-space:pre-wrap;\">%5</pre></td></tr></table>"_s
                     .arg(m_codeBg)
                     .arg(id)
                     .arg(m_muted, label, highlightCodeHtml(code, m_codeLang, m_opt.dark));
    }

    const RenderOptions &m_opt;
    QStringList *m_code;
    QString m_out;
    int m_baseOffset = 0;

    QColor m_text, m_base;
    QString m_codeBg, m_border, m_muted, m_mark;

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
