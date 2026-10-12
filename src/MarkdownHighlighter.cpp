#include "MarkdownHighlighter.h"

#include "Markdown.h"

#include <QFontDatabase>
#include <QPalette>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *doc)
    : QSyntaxHighlighter(doc)
{
}

void MarkdownHighlighter::setTheme(const QPalette &pal, const QFont &font)
{
    const QColor text = pal.color(QPalette::Text);
    const QColor base = pal.color(QPalette::Base);
    const QColor link = pal.color(QPalette::Link);
    const QColor muted = mix(text, base, 0.5);
    const bool dark = base.lightness() < 128;
    const double pt = font.pointSizeF();
    static const double scale[6] = {1.75, 1.45, 1.25, 1.1, 1.0, 1.0};

    for (int i = 0; i < 6; ++i) {
        m_heading[i] = {};
        m_heading[i].setFontPointSize(pt * scale[i]);
        m_heading[i].setFontWeight(QFont::Bold);
    }
    m_markup = {};
    m_markup.setForeground(muted);

    QTextCharFormat fixed;
    fixed.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
    fixed.setFontPointSize(pt * 0.92);

    m_code = fixed;
    m_code.setBackground(mix(base, text, dark ? 0.12 : 0.07));
    m_table = fixed;
    m_fenced = fixed; // block background is painted by Editor

    m_link = {};
    m_link.setForeground(link);
    m_quote = {};
    m_quote.setForeground(mix(text, base, 0.2));
    m_callout = {};
    m_callout.setForeground(link);
    m_callout.setFontWeight(QFont::Bold);
    m_list = {};
    m_list.setForeground(link);
    m_done = {};
    m_done.setForeground(muted);
    m_done.setFontStrikeOut(true);
    m_bold = {};
    m_bold.setFontWeight(QFont::Bold);
    m_italic = {};
    m_italic.setFontItalic(true);
    m_strike = {};
    m_strike.setFontStrikeOut(true);
    m_mark = {};
    m_mark.setBackground(mix(base, QColor(u"#ffd400"_s), dark ? 0.35 : 0.45));
    m_html = {};
    m_html.setForeground(mix(text, QColor(u"#e06c75"_s), 0.6));
    rehighlight();
}

void MarkdownHighlighter::apply(int start, int length, const QTextCharFormat &fmt)
{
    for (int i = start; i < start + length; ++i) {
        QTextCharFormat f = format(i);
        f.merge(fmt);
        setFormat(i, 1, f);
    }
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    using namespace BlockState;
    static const QRegularExpression fence(u"^\\s*(`{3,}|~{3,})"_s);
    static const QRegularExpression heading(u"^(#{1,6})[ \\t]"_s);
    static const QRegularExpression hr(u"^ {0,3}([-*_])( *\\1){2,} *$"_s);
    static const QRegularExpression quote(u"^\\s*(>[ >]*)"_s);
    static const QRegularExpression callout(u"^\\s*>\\s*\\[![\\w-]+\\][+-]?.*$"_s);
    static const QRegularExpression list(u"^(\\s*)([-*+]|\\d+[.)])(\\s+)(\\[[ xX]\\]\\s)?"_s);

    const int prev = qMax(0, previousBlockState());
    const int len = int(text.size());

    if (currentBlock().blockNumber() == 0 && text == u"---") {
        setFormat(0, len, m_markup);
        setCurrentBlockState(Frontmatter);
        return;
    }
    if (prev & Frontmatter) {
        setFormat(0, len, m_markup);
        setCurrentBlockState(text == u"---" || text == u"..." ? 0 : Frontmatter);
        return;
    }

    if (const auto m = fence.match(text); m.hasMatch()) {
        const bool tilde = m.captured(1).startsWith(u'~');
        int state = prev;
        if (!(prev & InFence))
            state = InFence | (tilde ? Tilde : 0) | FenceLine;
        else if (tilde == bool(prev & Tilde) && text.mid(m.capturedEnd()).trimmed().isEmpty())
            state = FenceLine;
        setFormat(0, len, m_fenced);
        apply(0, len, m_markup);
        setCurrentBlockState(state);
        return;
    }
    if (prev & InFence) {
        setFormat(0, len, m_fenced);
        setCurrentBlockState(prev & (InFence | Tilde));
        return;
    }
    setCurrentBlockState(0);

    if (const auto m = heading.match(text); m.hasMatch()) {
        const int level = int(m.capturedLength(1));
        setFormat(0, len, m_heading[level - 1]);
        apply(0, int(m.capturedLength(0)), m_markup);
        highlightInline(text, int(m.capturedLength(0)));
        return;
    }
    if (hr.match(text).hasMatch()) {
        setFormat(0, len, m_markup);
        return;
    }
    if (text.trimmed().startsWith(u'|')) {
        setFormat(0, len, m_table);
        for (int i = 0; i < len; ++i)
            if (text[i] == u'|')
                apply(i, 1, m_markup);
        highlightInline(text, 0);
        return;
    }

    int from = 0;
    if (const auto m = quote.match(text); m.hasMatch()) {
        setFormat(0, len, m_quote);
        apply(0, int(m.capturedEnd(1)), m_markup);
        from = int(m.capturedEnd(1));
        if (callout.match(text).hasMatch())
            apply(from, len - from, m_callout);
    }
    if (const auto m = list.match(text, from, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        m.hasMatch()) {
        apply(int(m.capturedStart(2)), int(m.capturedLength(2)), m_list);
        if (m.capturedLength(4)) {
            apply(int(m.capturedStart(4)), 3, m_list);
            const QChar mark = text[m.capturedStart(4) + 1];
            if (mark != u' ')
                apply(int(m.capturedEnd(0)), len - int(m.capturedEnd(0)), m_done);
        }
        from = int(m.capturedEnd(0));
    }
    highlightInline(text, from);
}

void MarkdownHighlighter::highlightInline(const QString &text, int from)
{
    static const QRegularExpression code(u"(`+)(.+?)\\1"_s);
    static const QRegularExpression wiki(u"!?\\[\\[[^\\[\\]\\n]+\\]\\]"_s);
    static const QRegularExpression mdlink(u"!?\\[([^\\]\\n]*)\\]\\(([^)\\n]*)\\)"_s);
    static const QRegularExpression url(u"(?<![(<\"])\\bhttps?://[^\\s)>\\]]+"_s);
    static const QRegularExpression bold(u"(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1"_s);
    static const QRegularExpression italic(u"(?<![*\\w])\\*(?=[^\\s*])(.+?)(?<=[^\\s*])\\*(?![*\\w])|(?<![_\\w])_(?=[^\\s_])(.+?)(?<=[^\\s_])_(?![_\\w])"_s);
    static const QRegularExpression strike(u"~~(?=\\S)(.+?)(?<=\\S)~~"_s);
    static const QRegularExpression mark(u"==(?=\\S)(.+?)(?<=\\S)=="_s);
    static const QRegularExpression html(u"</?[a-zA-Z][^>\\n]*>"_s);

    QList<QPair<int, int>> codeSpans;
    for (auto it = code.globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), l = int(m.capturedLength());
        codeSpans.append({s, s + l});
        apply(s, l, m_code);
        apply(s, int(m.capturedLength(1)), m_markup);
        apply(int(m.capturedEnd(2)), int(m.capturedLength(1)), m_markup);
    }
    auto inCode = [&](int pos) {
        for (const auto &[a, b] : std::as_const(codeSpans))
            if (pos >= a && pos < b)
                return true;
        return false;
    };
    auto each = [&](const QRegularExpression &re, auto fn) {
        for (auto it = re.globalMatch(text, from); it.hasNext();) {
            const auto m = it.next();
            if (!inCode(int(m.capturedStart())))
                fn(m);
        }
    };

    each(bold, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart()), int(m.capturedLength()), m_bold);
        apply(int(m.capturedStart()), 2, m_markup);
        apply(int(m.capturedEnd()) - 2, 2, m_markup);
    });
    each(italic, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart()), int(m.capturedLength()), m_italic);
        apply(int(m.capturedStart()), 1, m_markup);
        apply(int(m.capturedEnd()) - 1, 1, m_markup);
    });
    each(strike, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart()), int(m.capturedLength()), m_strike);
        apply(int(m.capturedStart()), 2, m_markup);
        apply(int(m.capturedEnd()) - 2, 2, m_markup);
    });
    each(mark, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart(1)), int(m.capturedLength(1)), m_mark);
        apply(int(m.capturedStart()), 2, m_markup);
        apply(int(m.capturedEnd()) - 2, 2, m_markup);
    });
    each(html, [&](const QRegularExpressionMatch &m) { apply(int(m.capturedStart()), int(m.capturedLength()), m_html); });
    each(url, [&](const QRegularExpressionMatch &m) { apply(int(m.capturedStart()), int(m.capturedLength()), m_link); });
    each(mdlink, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart()), int(m.capturedLength()), m_markup);
        apply(int(m.capturedStart(1)), int(m.capturedLength(1)), m_link);
    });
    each(wiki, [&](const QRegularExpressionMatch &m) {
        apply(int(m.capturedStart()), int(m.capturedLength()), m_link);
        const int open = text[m.capturedStart()] == u'!' ? 3 : 2;
        apply(int(m.capturedStart()), open, m_markup);
        apply(int(m.capturedEnd()) - 2, 2, m_markup);
    });
}
