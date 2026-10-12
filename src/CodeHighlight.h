#pragma once

#include <QString>

// Returns `code` as HTML (escaped, with colour spans when KSyntaxHighlighting
// is available and knows `lang`). Lines are separated by '\n'.
QString highlightCodeHtml(const QString &code, const QString &lang, bool dark);
