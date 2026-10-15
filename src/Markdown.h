#pragma once

#include <QPalette>
#include <QSet>
#include <QStringList>

class Vault;

struct RenderOptions {
    const Vault *vault = nullptr;
    QString notePath;
    int contentWidth = 720; // px, images are scaled down to fit
    QPalette palette;
    bool dark = false;
    int depth = 0;          // embed nesting
    QSet<int> toggledCallouts; // foldable callouts clicked open or shut, by order in the note
};

struct RenderResult {
    QString html;
    QStringList codeBlocks; // referenced by "copy:<index>" links
};

// Links in the HTML: wiki:<target>[#heading], task:<utf8 offset>, copy:<n>, callout:<n> and plain URLs.
RenderResult renderMarkdown(const QString &text, const RenderOptions &opt);
QString renderTableCell(const QString &cell, const RenderOptions &opt);
QString markdownStyleSheet(const QPalette &pal, double basePointSize);
QString headingAnchor(const QString &heading);
struct CalloutStyle {
    QString color;
    QString icon; // :/icons/<icon>.svg
};
CalloutStyle calloutStyle(const QString &type);
QColor mix(const QColor &a, const QColor &b, double t);
