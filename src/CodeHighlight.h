#pragma once

#include <QString>

// Escaped HTML, coloured when KSyntaxHighlighting is available and knows `lang`.
QString highlightCodeHtml(const QString &code, const QString &lang, bool dark);
