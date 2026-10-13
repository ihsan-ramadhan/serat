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

    // Line markup shows on the cursor's lines, inline markup only on the element it touches.
    void setLivePreview(bool on) { m_live = on; }
    void setRawRange(int fromBlock, int fromCol, int toBlock, int toCol);
    bool hidesMarkup(int blockNumber) const { return m_live && (blockNumber < m_rawFrom || blockNumber > m_rawTo); }

protected:
    void highlightBlock(const QString &text) override;

private:
    // Ghost keeps the width so the editor can paint a bullet, checkbox, quote bar or rule there.
    enum Hide { Collapse, Ghost };
    void markup(int start, int length, Hide how = Collapse);
    void inlineMarkup(int start, int length, int elementStart, int elementEnd);
    void apply(int start, int length, const QTextCharFormat &fmt);
    void highlightInline(const QString &text, int from);

    QTextCharFormat m_heading[6];
    QTextCharFormat m_markup, m_code, m_fenced, m_link, m_quote, m_callout, m_list, m_done;
    QTextCharFormat m_bold, m_italic, m_strike, m_mark, m_table, m_html;
    QTextCharFormat m_collapse, m_ghost;
    QFont m_font;
    bool m_live = false;
    bool m_hide = false;
    int m_rawFrom = -1, m_rawTo = -1;
    int m_colFrom = -1, m_colTo = -1;
    int m_block = -1;
};
