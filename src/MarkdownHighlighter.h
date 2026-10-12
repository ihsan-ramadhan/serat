#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// Block state bits, also read by Editor to paint code block backgrounds.
namespace BlockState {
constexpr int InFence = 1;
constexpr int Tilde = 2;
constexpr int FenceLine = 4;
constexpr int Frontmatter = 8;
}

class MarkdownHighlighter : public QSyntaxHighlighter
{
public:
    explicit MarkdownHighlighter(QTextDocument *doc);
    void setTheme(const QPalette &pal, const QFont &font);

protected:
    void highlightBlock(const QString &text) override;

private:
    void apply(int start, int length, const QTextCharFormat &fmt);
    void highlightInline(const QString &text, int from);

    QTextCharFormat m_heading[6];
    QTextCharFormat m_markup, m_code, m_fenced, m_link, m_quote, m_callout, m_list, m_done;
    QTextCharFormat m_bold, m_italic, m_strike, m_mark, m_table, m_html;
};
