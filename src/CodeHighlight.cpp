#include "CodeHighlight.h"

#ifdef SERAT_HAVE_KSH

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <QHash>

using namespace Qt::StringLiterals;
namespace KSH = KSyntaxHighlighting;

namespace {

KSH::Repository &repository()
{
    static KSH::Repository repo;
    return repo;
}

KSH::Definition definitionFor(const QString &lang)
{
    static const QHash<QString, QString> aliases = {
        {u"sh"_s, u"x.sh"_s},          {u"shell"_s, u"x.sh"_s},  {u"console"_s, u"x.sh"_s},
        {u"zsh"_s, u"x.zsh"_s},        {u"ps1"_s, u"x.ps1"_s},   {u"powershell"_s, u"x.ps1"_s},
        {u"pwsh"_s, u"x.ps1"_s},       {u"env"_s, u"x.env"_s},   {u"dotenv"_s, u"x.env"_s},
        {u"conf"_s, u"x.conf"_s},      {u"yml"_s, u"x.yaml"_s},  {u"py"_s, u"x.py"_s},
        {u"js"_s, u"x.js"_s},          {u"ts"_s, u"x.ts"_s},     {u"tsx"_s, u"x.tsx"_s},
        {u"jsx"_s, u"x.jsx"_s},        {u"rs"_s, u"x.rs"_s},     {u"md"_s, u"x.md"_s},
        {u"cs"_s, u"x.cs"_s},          {u"kt"_s, u"x.kt"_s},     {u"dockerfile"_s, u"Dockerfile"_s},
    };
    const QString l = lang.toLower();
    auto &repo = repository();
    if (const auto it = aliases.constFind(l); it != aliases.constEnd()) {
        const auto def = repo.definitionForFileName(*it);
        if (def.isValid())
            return def;
    }
    const auto defs = repo.definitions();
    for (const auto &def : defs)
        if (def.name().toLower() == l)
            return def;
    return repo.definitionForFileName(u"x."_s + l);
}

class HtmlHighlighter : public KSH::AbstractHighlighter
{
public:
    QString run(const QString &code)
    {
        QString out;
        KSH::State state;
        const QStringList lines = code.split(u'\n');
        for (qsizetype i = 0; i < lines.size(); ++i) {
            m_line = lines[i];
            m_out.clear();
            m_pos = 0;
            state = highlightLine(m_line, state);
            if (m_pos < m_line.size())
                m_out += m_line.mid(m_pos).toHtmlEscaped();
            if (i)
                out += u'\n';
            out += m_out;
        }
        return out;
    }

protected:
    void applyFormat(int offset, int length, const KSH::Format &format) override
    {
        if (offset > m_pos)
            m_out += m_line.mid(m_pos, offset - m_pos).toHtmlEscaped();
        const QString piece = m_line.mid(offset, length).toHtmlEscaped();
        m_pos = offset + length;
        if (format.isDefaultTextStyle(theme())) {
            m_out += piece;
            return;
        }
        QString style;
        if (format.hasTextColor(theme()))
            style += u"color:%1;"_s.arg(format.textColor(theme()).name());
        if (format.isBold(theme()))
            style += u"font-weight:600;"_s;
        if (format.isItalic(theme()))
            style += u"font-style:italic;"_s;
        m_out += style.isEmpty() ? piece : u"<span style=\"%1\">%2</span>"_s.arg(style, piece);
    }

private:
    QString m_line;
    QString m_out;
    int m_pos = 0;
};

} // namespace

QString highlightCodeHtml(const QString &code, const QString &lang, bool dark)
{
    if (lang.isEmpty())
        return code.toHtmlEscaped();
    const auto def = definitionFor(lang);
    if (!def.isValid())
        return code.toHtmlEscaped();
    HtmlHighlighter h;
    h.setDefinition(def);
    h.setTheme(repository().defaultTheme(dark ? KSH::Repository::DarkTheme : KSH::Repository::LightTheme));
    return h.run(code);
}

#else

QString highlightCodeHtml(const QString &code, const QString &, bool)
{
    return code.toHtmlEscaped();
}

#endif
